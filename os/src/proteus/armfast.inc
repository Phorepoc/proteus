// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armfast.inc -- OURS (Proteus speed work, docs/sparkle-gl-run.md "Interpreter speed"): the fast ARM-mode execution loop, #included at the end of armcpu.cpp.
//
// Step() decodes every instruction with a chain of mask compares and keeps all state (registers, flags, pc, step counter) in the object, so a guest
// instruction cost ~200 host instructions (callgrind) and, under QEMU TCG where every load/store of the interpreter's own data is a softmmu
// access, ~10x more time than on the host. This loop:
//   * dispatches on ONE table lookup (ins bits 27:20 and 7:4 -> a class byte; the data-processing class carries op, S and operand kind, so
//     each of the 96 ALU variants is a template instantiation with the operation constant-folded) and one switch;
//   * keeps pc, the flags (as 0/1 words), the step counter and the memory-region description in locals for the whole batch;
//   * checks a condition with one 16-bit mask lookup (and not at all for AL);
//   * reads and writes the guest region directly (one range compare + one page-map byte), falling back to the exact Read32/Write32 for anything
//     else (windows, page-crossing accesses, faults, watchpoints);
//   * leaves everything it does not know (coprocessor, SWI, misc group, cond 0xF, Thumb, traps, breakpoints, foreign pc) to Step(), unchanged.
// The semantics are those of Step(): tests/isa (qemu-arm differential) and the tests/bench checksum run through both.
//
// C++03 (the x32 Phorepoc build is -std=gnu++03): no lambdas, no auto.
#ifndef PROTEUS_ARMFAST_INC
#define PROTEUS_ARMFAST_INC

#define PH_INLINE inline __attribute__((always_inline))
#define PH_LIKELY(x) __builtin_expect(!!(x), 1)
#define PH_UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace proteus
{
struct FastCore
{
	// the working state of one batch
	struct FS
	{
		CArmCpu* c;
		uint32_t* r;
		uint8_t* host;
		const uint8_t* pmap;
		uint32_t base, size;
		uint32_t np;			// the address of the NEXT instruction (= R[15] while one executes)
		uint32_t cur;			// the address of the instruction being executed
		uint32_t fl;			// the flags: N = bit 3, Z = bit 2, C = bit 1, V = bit 0 (the index of sCond)
		bool stop, thumb, watch;
	};

	static uint8_t sCls[4096];
	static uint16_t sCond[16];
	static bool sInit;

	enum
	{
		KSlow = 0,
		KDp = 1,					// 1 + kind * 32 + op * 2 + S, kind 0 = immediate, 1 = register shifted by an immediate, 2 = register shifted by a register
		KLs = 100,					// 100 + (ins bits 24:20 = P U B W L) + 32 * register-offset
		KLdm = 170, KB, KBl, KBx, KMul, KMull, KHalf, KLdrex, KStrex, KMovw, KMovt, KMedia
	};

	static void Init()
	{
		for (uint32_t c = 0; c < 15; ++c)
			for (uint32_t f = 0; f < 16; ++f)
			{
				const bool N = (f >> 3) & 1, Z = (f >> 2) & 1, C = (f >> 1) & 1, V = f & 1;
				bool t;
				switch (c)
				{
				case 0: t = Z; break;
				case 1: t = !Z; break;
				case 2: t = C; break;
				case 3: t = !C; break;
				case 4: t = N; break;
				case 5: t = !N; break;
				case 6: t = V; break;
				case 7: t = !V; break;
				case 8: t = C && !Z; break;
				case 9: t = !C || Z; break;
				case 10: t = N == V; break;
				case 11: t = N != V; break;
				case 12: t = !Z && N == V; break;
				case 13: t = Z || N != V; break;
				default: t = true; break;
				}
				if (t) sCond[c] |= (uint16_t)(1u << f);
			}
		for (uint32_t i = 0; i < 4096; ++i)
		{
			const uint32_t hi = i >> 4, lo = i & 15, type = hi >> 5, op = (hi >> 1) & 15, S = hi & 1;
			uint8_t k = KSlow;
			switch (type)
			{
			case 0:
				if ((lo & 9) == 9)
				{
					if (lo == 9)
					{
						if (hi <= 3) k = KMul;
						else if (hi >= 8 && hi <= 15) k = KMull;
						else if (hi >= 0x18 && hi <= 0x1F) k = (hi & 1) ? KLdrex : KStrex;
					}
					else
						k = KHalf;
				}
				else if ((hi & 0x19) == 0x10)		// TST..CMN without S: the misc group (BX/BLX register are the only ones taken here)
				{
					if (hi == 0x12 && (lo == 1 || lo == 3)) k = KBx;
				}
				else
					k = (uint8_t)(KDp + ((lo & 1) ? 64 : 32) + op * 2 + S);
				break;
			case 1:
				if (hi == 0x30) k = KMovw;
				else if (hi == 0x34) k = KMovt;
				else if ((hi & 0x19) != 0x10) k = (uint8_t)(KDp + op * 2 + S);
				break;
			case 2: k = (uint8_t)(KLs + (hi & 31)); break;		// L = bit 20, B = bit 22
			case 3: k = (lo & 1) ? (uint8_t)KMedia : (uint8_t)(KLs + 32 + (hi & 31)); break;
			case 4: k = KLdm; break;
			case 5: k = (hi & 0x10) ? KBl : KB; break;
			default: break;
			}
			sCls[i] = k;
		}
		sInit = true;
	}

	static PH_INLINE uint32_t Pop16(uint32_t v)
	{
		v = v - ((v >> 1) & 0x5555);
		v = (v & 0x3333) + ((v >> 2) & 0x3333);
		v = (v + (v >> 4)) & 0x0F0F;
		return (v + (v >> 8)) & 0x1F;
	}

	static PH_INLINE uint32_t Load(const CArmCpu& c) { return ((c.iN & 1) << 3) | ((c.iZ & 1) << 2) | ((c.iC & 1) << 1) | (c.iV & 1); }
	static PH_INLINE void Store(CArmCpu& c, uint32_t f) { c.iN = f >> 3; c.iZ = (f >> 2) & 1; c.iC = (f >> 1) & 1; c.iV = f & 1; }

	// ---- registers ------------------------------------------------------------------------------------------------------------------
	static PH_INLINE uint32_t Rg(const FS& s, uint32_t n, uint32_t extra) { return n == 15 ? s.np + extra : s.r[n]; }

	// ---- guest memory ---------------------------------------------------------------------------------------------------------------
	// (these take the pieces, not the FS: a FS whose address escaped into a call would live in memory instead of registers)
	static __attribute__((noinline, cold)) uint32_t SlowRd(CArmCpu* c, uint32_t np, uint32_t cur, uint32_t a, int size)
	{
		c->R[15] = np;
		c->iLastPc = cur;
		return size == 4 ? c->Read32(a) : size == 2 ? c->Read16(a) : c->Read8(a);
	}
	static __attribute__((noinline, cold)) void SlowWr(CArmCpu* c, uint32_t np, uint32_t cur, uint32_t a, uint32_t v, int size)
	{
		c->R[15] = np;
		c->iLastPc = cur;
		if (size == 4) c->Write32(a, v); else if (size == 2) c->Write16(a, (uint16_t)v); else c->Write8(a, (uint8_t)v);
	}
	static PH_INLINE uint32_t SlowRdS(FS& s, uint32_t a, int size)
	{
		const uint32_t v = SlowRd(s.c, s.np, s.cur, a, size);
		if (s.c->iHalt != ERunning) s.stop = true;
		return v;
	}
	static PH_INLINE void SlowWrS(FS& s, uint32_t a, uint32_t v, int size)
	{
		SlowWr(s.c, s.np, s.cur, a, v, size);
		if (s.c->iHalt != ERunning) s.stop = true;
	}
	static PH_INLINE uint32_t Rd32(FS& s, uint32_t a)
	{
		const uint32_t off = a - s.base;
		if (PH_LIKELY(off <= s.size - 4 && (off & 0xFFF) <= 0xFFC && (!s.pmap || s.pmap[off >> 12])))
		{
			uint32_t v;
			__builtin_memcpy(&v, s.host + off, 4);
			return v;
		}
		return SlowRdS(s, a, 4);
	}
	static PH_INLINE uint32_t Rd8(FS& s, uint32_t a)
	{
		const uint32_t off = a - s.base;
		if (PH_LIKELY(off < s.size && (!s.pmap || s.pmap[off >> 12]))) return s.host[off];
		return SlowRdS(s, a, 1);
	}
	static PH_INLINE uint32_t Rd16(FS& s, uint32_t a)
	{
		const uint32_t off = a - s.base;
		if (PH_LIKELY(off <= s.size - 2 && (off & 0xFFF) <= 0xFFE && (!s.pmap || s.pmap[off >> 12])))
		{
			uint16_t v;
			__builtin_memcpy(&v, s.host + off, 2);
			return v;
		}
		return SlowRdS(s, a, 2);
	}
	static PH_INLINE void Wr32(FS& s, uint32_t a, uint32_t v)
	{
		const uint32_t off = a - s.base;
		if (PH_LIKELY(!s.watch && off <= s.size - 4 && (off & 0xFFF) <= 0xFFC && (!s.pmap || s.pmap[off >> 12])))
		{
			__builtin_memcpy(s.host + off, &v, 4);
			return;
		}
		SlowWrS(s, a, v, 4);
	}
	static PH_INLINE void Wr8(FS& s, uint32_t a, uint32_t v)
	{
		const uint32_t off = a - s.base;
		if (PH_LIKELY(!s.watch && off < s.size && (!s.pmap || s.pmap[off >> 12]))) { s.host[off] = (uint8_t)v; return; }
		SlowWrS(s, a, v, 1);
	}
	static PH_INLINE void Wr16(FS& s, uint32_t a, uint32_t v)
	{
		const uint32_t off = a - s.base;
		if (PH_LIKELY(!s.watch && off <= s.size - 2 && (off & 0xFFF) <= 0xFFE && (!s.pmap || s.pmap[off >> 12])))
		{
			const uint16_t h = (uint16_t)v;
			__builtin_memcpy(s.host + off, &h, 2);
			return;
		}
		SlowWrS(s, a, v, 2);
	}

	// BX semantics
	static PH_INLINE void Branch(FS& s, uint32_t t)
	{
		s.np = t & ~1u;
		s.thumb = (t & 1) != 0;
	}

	// ---- Thumb helpers (flags in the one nibble) ----
	static PH_INLINE void ThNz(FS& s, uint32_t v) { s.fl = (s.fl & 3) | ((v >> 31) << 3) | ((uint32_t)(v == 0) << 2); }
	static PH_INLINE uint32_t ThAdd(FS& s, uint32_t a, uint32_t b, uint32_t cin)
	{
		const uint64_t res = (uint64_t)a + b + cin;
		const uint32_t v = (uint32_t)res;
		s.fl = ((v >> 31) << 3) | ((uint32_t)(v == 0) << 2) | (((uint32_t)(res >> 32) & 1) << 1) | (((~(a ^ b) & (a ^ v)) >> 31) & 1);
		return v;
	}
	static PH_INLINE void SetC(FS& s, uint32_t c) { s.fl = (s.fl & ~2u) | ((c & 1) << 1); }
	static PH_INLINE uint32_t ThShift(FS& s, uint32_t type, uint32_t v, uint32_t amt)
	{
		if (amt == 0) return v;
		if (type == 0) { if (amt < 32) { SetC(s, v >> (32 - amt)); return v << amt; } SetC(s, amt == 32 ? v & 1 : 0); return 0; }
		if (type == 1) { if (amt < 32) { SetC(s, v >> (amt - 1)); return v >> amt; } SetC(s, amt == 32 ? v >> 31 : 0); return 0; }
		if (type == 2) { if (amt < 32) { SetC(s, v >> (amt - 1)); return (uint32_t)((int32_t)v >> amt); } SetC(s, v >> 31); return (v >> 31) ? 0xFFFFFFFFu : 0; }
		const uint32_t a = amt & 31;
		if (!a) { SetC(s, v >> 31); return v; }
		SetC(s, v >> (a - 1));
		return (v >> a) | (v << (32 - a));
	}

	// ---- data processing --------------------------------------------------------------------------------------------------------------
	// the register second operand (Operand2 for bit 25 clear); shc = shifter carry out
	template<bool REGSHIFT>
	static PH_INLINE uint32_t Shifted(const FS& s, uint32_t ins, uint32_t& shc)
	{
		uint32_t rm = Rg(s, ins & 0xF, REGSHIFT ? 8 : 4);
		uint32_t amt = REGSHIFT ? (Rg(s, (ins >> 8) & 0xF, 4) & 0xFF) : ((ins >> 7) & 0x1F);
		const uint32_t type = (ins >> 5) & 3;
		if (type == 0)			// LSL
		{
			if (amt == 0) return rm;
			if (amt < 32) { shc = (rm >> (32 - amt)) & 1; return rm << amt; }
			shc = amt == 32 ? (rm & 1) : 0;
			return 0;
		}
		if (type == 1)			// LSR
		{
			if (!REGSHIFT && amt == 0) amt = 32;
			if (amt == 0) return rm;
			if (amt < 32) { shc = (rm >> (amt - 1)) & 1; return rm >> amt; }
			shc = amt == 32 ? (rm >> 31) & 1 : 0;
			return 0;
		}
		if (type == 2)			// ASR
		{
			if (!REGSHIFT && amt == 0) amt = 32;
			if (amt == 0) return rm;
			if (amt < 32) { shc = (rm >> (amt - 1)) & 1; return (uint32_t)((int32_t)rm >> amt); }
			shc = (rm >> 31) & 1;
			return (rm >> 31) ? 0xFFFFFFFFu : 0;
		}
		if (!REGSHIFT && amt == 0)	// RRX
		{
			const uint32_t r = (rm >> 1) | (((s.fl >> 1) & 1) << 31);
			shc = rm & 1;
			return r;
		}
		if (amt == 0) return rm;
		amt &= 31;
		if (amt == 0) { shc = (rm >> 31) & 1; return rm; }
		shc = (rm >> (amt - 1)) & 1;
		return (rm >> amt) | (rm << (32 - amt));
	}

	template<int OP, int S, int KIND>
	static PH_INLINE void Dp(FS& s, uint32_t ins)
	{
		const uint32_t rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
		uint32_t shc = (s.fl >> 1) & 1, b;
		if (KIND == 0)
		{
			const uint32_t rot = ((ins >> 8) & 0xF) * 2;
			b = ins & 0xFF;
			if (rot)
			{
				b = (b >> rot) | (b << (32 - rot));
				shc = b >> 31;
			}
		}
		else if (KIND == 1) b = Shifted<false>(s, ins, shc);
		else b = Shifted<true>(s, ins, shc);
		const uint32_t a = Rg(s, rn, KIND == 2 ? 8 : 4);
		uint64_t res = 0;
		const bool write = !(OP >= 8 && OP <= 11);
		const bool arith = (OP >= 2 && OP <= 7) || OP == 10 || OP == 11;
		uint32_t r;
		switch (OP)
		{
		case 0: case 8: r = a & b; break;
		case 1: case 9: r = a ^ b; break;
		case 2: case 10: res = (uint64_t)a + (uint64_t)(~b) + 1; r = (uint32_t)res; break;
		case 3: res = (uint64_t)b + (uint64_t)(~a) + 1; r = (uint32_t)res; break;
		case 4: case 11: res = (uint64_t)a + b; r = (uint32_t)res; break;
		case 5: res = (uint64_t)a + b + ((s.fl >> 1) & 1); r = (uint32_t)res; break;
		case 6: res = (uint64_t)a + (uint64_t)(~b) + ((s.fl >> 1) & 1); r = (uint32_t)res; break;
		case 7: res = (uint64_t)b + (uint64_t)(~a) + ((s.fl >> 1) & 1); r = (uint32_t)res; break;
		case 12: r = a | b; break;
		case 13: r = b; break;
		case 14: r = a & ~b; break;
		default: r = ~b; break;
		}
		if (S && !(rd == 15 && write))
		{
			const uint32_t nz = ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2);
			if (arith)
			{
				const uint32_t x = (OP == 3 || OP == 7) ? b : a;
				const uint32_t y = (OP == 2 || OP == 6 || OP == 10) ? ~b : (OP == 3 || OP == 7) ? ~a : b;
				s.fl = nz | (((uint32_t)(res >> 32) & 1) << 1) | (((~(x ^ y) & (x ^ r)) >> 31) & 1);
			}
			else
				s.fl = nz | (shc << 1) | (s.fl & 1);
		}
		if (write)
		{
			if (rd == 15) Branch(s, r);
			else s.r[rd] = r;
		}
	}

	// ---- LDR / STR / LDRB / STRB --------------------------------------------------------------------------------------------------------
	template<int L, int B, int REG, int P, int U, int W>
	static PH_INLINE void Ls(FS& s, uint32_t ins)
	{
		const bool p = P, u = U, w = W;
		const uint32_t rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
		uint32_t off;
		if (!REG) off = ins & 0xFFF;
		else
		{
			const uint32_t rm = Rg(s, ins & 0xF, 4), amt = (ins >> 7) & 0x1F, type = (ins >> 5) & 3;
			if (type == 0) off = rm << amt;
			else if (type == 1) off = amt ? rm >> amt : 0;
			else if (type == 2) off = amt ? (uint32_t)((int32_t)rm >> amt) : (uint32_t)((int32_t)rm >> 31);
			else off = amt ? (rm >> amt) | (rm << (32 - amt)) : (rm >> 1) | (((s.fl >> 1) & 1) << 31);
		}
		const uint32_t base = Rg(s, rn, 4);
		const uint32_t moved = u ? base + off : base - off;
		const uint32_t addr = p ? moved : base;
		if (L)
		{
			uint32_t v;
			if (B) v = Rd8(s, addr);
			else
			{
				v = Rd32(s, addr & ~3u);
				const uint32_t rot = 8 * (addr & 3);
				if (rot) v = (v >> rot) | (v << (32 - rot));
			}
			if (!p || w) s.r[rn] = moved;
			if (rd == 15) Branch(s, v);
			else s.r[rd] = v;
		}
		else
		{
			const uint32_t v = Rg(s, rd, 8);
			if (B) Wr8(s, addr, v);
			else Wr32(s, addr, v);
			if (!p || w) s.r[rn] = moved;
		}
	}

	template<int L>
	static PH_INLINE void Block(FS& s, uint32_t ins)
	{
		const bool p = ins & (1u << 24), u = ins & (1u << 23), w = ins & (1u << 21);
		const uint32_t rn = (ins >> 16) & 0xF, list = ins & 0xFFFF;
		const uint32_t n = Pop16(list);
		const uint32_t base = Rg(s, rn, 0);
		uint32_t addr = u ? (p ? base + 4 : base) : (p ? base - 4 * n : base - 4 * n + 4);
		const uint32_t wb = u ? base + 4 * n : base - 4 * n;
		uint32_t m = list;
		while (m)
		{
			const uint32_t i = (uint32_t)__builtin_ctz(m);
			m &= m - 1;
			if (L)
			{
				const uint32_t v = Rd32(s, addr);
				if (i == 15) Branch(s, v);
				else s.r[i] = v;
			}
			else
				Wr32(s, addr, i == 15 ? s.np + 8 : s.r[i]);
			addr += 4;
		}
		if (w && !(L && ((list >> rn) & 1)))
			s.r[rn] = wb;
	}

	// ---- the batch --------------------------------------------------------------------------------------------------------------------
#define PH_DPOP(K, O) \
	case KDp + (K) * 32 + (O) * 2:     Dp<O, 0, K>(s, ins); break; \
	case KDp + (K) * 32 + (O) * 2 + 1: Dp<O, 1, K>(s, ins); break;
#define PH_DPK(K) \
	PH_DPOP(K, 0) PH_DPOP(K, 1) PH_DPOP(K, 2) PH_DPOP(K, 3) PH_DPOP(K, 4) PH_DPOP(K, 5) PH_DPOP(K, 6) PH_DPOP(K, 7) \
	PH_DPOP(K, 8) PH_DPOP(K, 9) PH_DPOP(K, 10) PH_DPOP(K, 11) PH_DPOP(K, 12) PH_DPOP(K, 13) PH_DPOP(K, 14) PH_DPOP(K, 15)

#define PH_LS1(R, H) case KLs + (R) * 32 + (H): Ls<(H) & 1, ((H) >> 2) & 1, R, ((H) >> 4) & 1, ((H) >> 3) & 1, ((H) >> 1) & 1>(s, ins); break;
#define PH_LSA(R, H) PH_LS1(R, H) PH_LS1(R, (H) + 1) PH_LS1(R, (H) + 2) PH_LS1(R, (H) + 3) PH_LS1(R, (H) + 4) PH_LS1(R, (H) + 5) PH_LS1(R, (H) + 6) PH_LS1(R, (H) + 7)
#define PH_LSB(R) PH_LSA(R, 0) PH_LSA(R, 8) PH_LSA(R, 16) PH_LSA(R, 24)

	// Executes instructions until iSteps >= aLimit, the guest halts, or the next instruction needs the generic path (then true: the caller
	// runs one Step()). Precondition: no breakpoints. ARM and Thumb state alike.
	static bool Batch(CArmCpu& c, uint64_t aLimit)
	{
		if (!sInit) Init();
		FS s;
		s.c = &c;
		s.r = c.R;
		s.host = c.iHost;
		s.pmap = c.iPageMap;
		s.base = c.iBase;
		s.size = c.iSize;
		s.watch = c.iOnWatch != 0;
		s.np = c.R[15];
		s.cur = s.np;
		s.fl = Load(c);
		s.stop = false;
		s.thumb = c.iT != 0;
		uint64_t steps = c.iSteps;
		bool generic = false;
		while (steps < aLimit)
		{
			if (s.thumb)
			{
				// ---- Thumb-1 (thumb.cpp's ThumbStep, same semantics, on the batch's locals) ----
				const uint32_t toff = s.np - s.base;
				if (PH_UNLIKELY(toff > s.size - 2)) { generic = true; break; }
				uint32_t ins;
				{
					uint16_t h;
					__builtin_memcpy(&h, s.host + toff, 2);
					ins = h;
				}
				s.cur = s.np;
				s.np += 2;
				++steps;
				const uint32_t pcRead = s.np + 2;
				uint32_t* r = s.r;
				switch (ins >> 11)
				{
				case 0: case 1: case 2:				// format 1: LSL/LSR/ASR Rd, Rm, #imm5
				{
					const uint32_t type = ins >> 11, imm = (ins >> 6) & 31, rm = r[(ins >> 3) & 7];
					const uint32_t v = type == 0 ? ThShift(s, 0, rm, imm) : ThShift(s, type, rm, imm ? imm : 32);
					r[ins & 7] = v;
					ThNz(s, v);
					break;
				}
				case 3:								// format 2: ADD/SUB Rd, Rn, Rm|#imm3
				{
					const uint32_t a = r[(ins >> 3) & 7], b = (ins & (1u << 10)) ? (ins >> 6) & 7 : r[(ins >> 6) & 7];
					r[ins & 7] = (ins & (1u << 9)) ? ThAdd(s, a, ~b, 1) : ThAdd(s, a, b, 0);
					break;
				}
				case 4: case 5: case 6: case 7:		// format 3: MOV/CMP/ADD/SUB Rd, #imm8
				{
					const uint32_t rd = (ins >> 8) & 7, imm = ins & 0xFF, op = (ins >> 11) & 3;
					if (op == 0) { r[rd] = imm; ThNz(s, imm); }
					else if (op == 1) ThAdd(s, r[rd], ~imm, 1);
					else if (op == 2) r[rd] = ThAdd(s, r[rd], imm, 0);
					else r[rd] = ThAdd(s, r[rd], ~imm, 1);
					break;
				}
				case 8:
					if ((ins >> 10) == 0x10)		// format 4: ALU Rd, Rs
					{
						const uint32_t rd = ins & 7, rs = (ins >> 3) & 7, a = r[rd], b = r[rs];
						uint32_t v;
						switch ((ins >> 6) & 15)
						{
						case 0: v = a & b; ThNz(s, v); r[rd] = v; break;
						case 1: v = a ^ b; ThNz(s, v); r[rd] = v; break;
						case 2: v = ThShift(s, 0, a, b & 0xFF); ThNz(s, v); r[rd] = v; break;
						case 3: v = ThShift(s, 1, a, b & 0xFF); ThNz(s, v); r[rd] = v; break;
						case 4: v = ThShift(s, 2, a, b & 0xFF); ThNz(s, v); r[rd] = v; break;
						case 5: r[rd] = ThAdd(s, a, b, (s.fl >> 1) & 1); break;
						case 6: r[rd] = ThAdd(s, a, ~b, (s.fl >> 1) & 1); break;
						case 7: v = ThShift(s, 3, a, b & 0xFF); ThNz(s, v); r[rd] = v; break;
						case 8: ThNz(s, a & b); break;
						case 9: r[rd] = ThAdd(s, 0, ~b, 1); break;
						case 10: ThAdd(s, a, ~b, 1); break;
						case 11: ThAdd(s, a, b, 0); break;
						case 12: v = a | b; ThNz(s, v); r[rd] = v; break;
						case 13: v = a * b; ThNz(s, v); r[rd] = v; break;
						case 14: v = a & ~b; ThNz(s, v); r[rd] = v; break;
						default: v = ~b; ThNz(s, v); r[rd] = v; break;
						}
						break;
					}
					else							// format 5: hi-register ADD/CMP/MOV, BX/BLX
					{
						const uint32_t rd = (ins & 7) | (((ins >> 7) & 1) << 3), rm = (ins >> 3) & 15;
						const uint32_t b = rm == 15 ? pcRead : r[rm];
						const uint32_t op = (ins >> 8) & 3;
						if (op == 0) { const uint32_t v = (rd == 15 ? pcRead : r[rd]) + b; if (rd == 15) Branch(s, (v & ~1u) | 1); else r[rd] = v; }
						else if (op == 1) ThAdd(s, rd == 15 ? pcRead : r[rd], ~b, 1);
						else if (op == 2) { if (rd == 15) Branch(s, (b & ~1u) | 1); else r[rd] = b; }
						else
						{
							if (ins & 0x80) r[14] = s.np | 1;
							Branch(s, b);
						}
						break;
					}
				case 9:								// format 6: LDR Rd, [PC, #imm8*4]
					r[(ins >> 8) & 7] = Rd32(s, (pcRead & ~3u) + (ins & 0xFF) * 4);
					break;
				case 10: case 11:					// formats 7/8: load/store with register offset
				{
					const uint32_t addr = r[(ins >> 3) & 7] + r[(ins >> 6) & 7], rd = ins & 7;
					switch ((ins >> 9) & 7)
					{
					case 0: Wr32(s, addr, r[rd]); break;
					case 1: Wr16(s, addr, r[rd]); break;
					case 2: Wr8(s, addr, r[rd]); break;
					case 3: r[rd] = (uint32_t)(int32_t)(int8_t)Rd8(s, addr); break;
					case 4: r[rd] = Rd32(s, addr); break;
					case 5: r[rd] = Rd16(s, addr); break;
					case 6: r[rd] = Rd8(s, addr); break;
					default: r[rd] = (uint32_t)(int32_t)(int16_t)Rd16(s, addr); break;
					}
					break;
				}
				case 12: case 13: case 14: case 15:	// format 9: LDR/STR (word: imm5*4, byte: imm5)
				{
					const bool byte = ins & (1u << 12), load = ins & (1u << 11);
					const uint32_t rd = ins & 7, addr = r[(ins >> 3) & 7] + ((ins >> 6) & 31) * (byte ? 1 : 4);
					if (load) r[rd] = byte ? Rd8(s, addr) : Rd32(s, addr);
					else if (byte) Wr8(s, addr, r[rd]);
					else Wr32(s, addr, r[rd]);
					break;
				}
				case 16: case 17:					// format 10: LDRH/STRH imm5*2
				{
					const uint32_t rd = ins & 7, addr = r[(ins >> 3) & 7] + ((ins >> 6) & 31) * 2;
					if (ins & (1u << 11)) r[rd] = Rd16(s, addr); else Wr16(s, addr, r[rd]);
					break;
				}
				case 18: case 19:					// format 11: SP-relative LDR/STR
				{
					const uint32_t rd = (ins >> 8) & 7, addr = r[13] + (ins & 0xFF) * 4;
					if (ins & (1u << 11)) r[rd] = Rd32(s, addr); else Wr32(s, addr, r[rd]);
					break;
				}
				case 20: case 21:					// format 12: ADD Rd, PC|SP, #imm8*4
					r[(ins >> 8) & 7] = ((ins & (1u << 11)) ? r[13] : (pcRead & ~3u)) + (ins & 0xFF) * 4;
					break;
				case 22: case 23:
					if ((ins & 0xFF00) == 0xB000)	// format 13: ADD/SUB SP, #imm7*4
					{
						const uint32_t o = (ins & 0x7F) * 4;
						r[13] = (ins & 0x80) ? r[13] - o : r[13] + o;
						break;
					}
					if ((ins & 0xF600) == 0xB400)	// format 14: PUSH {rlist[, LR]} / POP {rlist[, PC]}
					{
						const uint32_t list = ins & 0xFF;
						const bool extra = ins & 0x100;
						if (!(ins & 0x800))
						{
							uint32_t a = r[13] - 4 * (Pop16(list) + (extra ? 1 : 0));
							r[13] = a;
							for (uint32_t m = list; m; m &= m - 1) { Wr32(s, a, r[__builtin_ctz(m)]); a += 4; }
							if (extra) Wr32(s, a, r[14]);
						}
						else
						{
							uint32_t a = r[13];
							for (uint32_t m = list; m; m &= m - 1) { r[__builtin_ctz(m)] = Rd32(s, a); a += 4; }
							uint32_t newPc = 0;
							if (extra) { newPc = Rd32(s, a); a += 4; }
							r[13] = a;
							if (extra) Branch(s, newPc);
						}
						break;
					}
					goto GenericPath;				// BKPT, undefined
				case 24: case 25:					// format 15: STMIA/LDMIA Rb!, {rlist}
				{
					const uint32_t rb = (ins >> 8) & 7, list = ins & 0xFF;
					uint32_t a = r[rb];
					if (ins & (1u << 11))
					{
						for (uint32_t m = list; m; m &= m - 1) { r[__builtin_ctz(m)] = Rd32(s, a); a += 4; }
						if (!(list & (1u << rb))) r[rb] = a;
					}
					else
					{
						for (uint32_t m = list; m; m &= m - 1) { Wr32(s, a, r[__builtin_ctz(m)]); a += 4; }
						r[rb] = a;
					}
					break;
				}
				case 26: case 27:					// format 16/17: B<cond> / SWI
				{
					const uint32_t tc = (ins >> 8) & 15;
					if (tc >= 0xE) goto GenericPath;	// SWI, undefined
					if ((sCond[tc] >> s.fl) & 1) Branch(s, (pcRead + ((int32_t)(int8_t)(ins & 0xFF) * 2)) | 1);
					break;
				}
				case 28:							// format 18: B
					Branch(s, (pcRead + ((int32_t)(ins << 21) >> 20)) | 1);
					break;
				case 29:							// BLX suffix: to ARM, word aligned
				{
					if (ins & 1) goto GenericPath;
					const uint32_t t = (r[14] + ((ins & 0x7FF) << 1)) & ~3u;
					r[14] = s.np | 1;
					Branch(s, t);
					break;
				}
				case 30:							// BL/BLX prefix: LR = PC + (offset_hi << 12)
					r[14] = pcRead + ((int32_t)(ins << 21) >> 9);
					break;
				default:							// BL suffix
				{
					const uint32_t t = r[14] + ((ins & 0x7FF) << 1);
					r[14] = s.np | 1;
					Branch(s, t | 1);
					break;
				}
				}
				if (PH_UNLIKELY(s.stop)) break;
				continue;
			}
			{
			uint32_t ins;
			const uint32_t off = s.np - s.base;
			if (PH_UNLIKELY(off > s.size - 4 || (s.np & 3))) { generic = true; break; }
			__builtin_memcpy(&ins, s.host + off, 4);
			s.cur = s.np;
			const uint32_t cond = ins >> 28;
			const uint32_t cls = sCls[((ins >> 16) & 0xFF0) | ((ins >> 4) & 0xF)];
			if (PH_UNLIKELY(cond == 0xF || cls == KSlow))
			{
				// the generic path: R[15] = this instruction, everything in the object
				c.R[15] = s.cur;
				Store(c, s.fl);
				c.iT = 0;
				c.iSteps = steps;
				++c.iStatSlow;
				c.Step();
				steps = c.iSteps;
				s.np = c.R[15];
				s.fl = Load(c);
				s.thumb = c.iT != 0;
				if (c.iHalt != ERunning || c.iBpCount) { s.stop = c.iHalt != ERunning; break; }
				s.pmap = c.iPageMap;
				s.watch = c.iOnWatch != 0;
				continue;
			}
			s.np += 4;
			++steps;
			if (cond != 14)
			{
				if (!((sCond[cond] >> s.fl) & 1)) continue;
			}
			switch (cls)
			{
			PH_DPK(0)
			PH_DPK(1)
			PH_DPK(2)
			PH_LSB(0)
			PH_LSB(1)
			case KLdm: if (ins & (1u << 20)) Block<1>(s, ins); else Block<0>(s, ins); break;
			case KB: s.np += 4 + ((int32_t)(ins << 8) >> 6); break;
			case KBl: s.r[14] = s.np; s.np += 4 + ((int32_t)(ins << 8) >> 6); break;
			case KBx:
			{
				if ((ins & 0x0FFFFFD0) != 0x012FFF10) goto GenericPath;
				const uint32_t m = ins & 0xF, t = m == 15 ? s.np : s.r[m];
				if (ins & 0x20) s.r[14] = s.np;			// BLX register: bit 5 of 0x...30 (BX is 0x...10)
				Branch(s, t);
				break;
			}
			case KMul:
			{
				const uint32_t rd = (ins >> 16) & 0xF, rn = (ins >> 12) & 0xF;
				const uint32_t m = ins & 0xF, q = (ins >> 8) & 0xF;
				uint32_t r = (m == 15 ? s.np : s.r[m]) * (q == 15 ? s.np : s.r[q]);
				if (ins & (1u << 21)) r += rn == 15 ? s.np : s.r[rn];
				s.r[rd] = r;
				if (ins & (1u << 20)) s.fl = (s.fl & 3) | ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2);
				break;
			}
			case KMull:
			{
				c.R[15] = s.np;
				Store(c, s.fl);
				c.Multiply(ins);
				s.np = c.R[15];
				s.fl = Load(c);
				break;
			}
			case KHalf:
				c.R[15] = s.np;
				c.iLastPc = s.cur;
				c.HalfwordTransfer(ins);
				s.np = c.R[15];
				if (c.iHalt != ERunning) s.stop = true;
				break;
			case KLdrex:
				if ((ins & 0x0F900FFF) != 0x01900F9F) goto GenericPath;
				{
					const uint32_t a = Rg(s, (ins >> 16) & 0xF, 0), sz = (ins >> 21) & 3, rt = (ins >> 12) & 0xF;
					c.iExcl = true;
					if (sz == 1)
					{
						const uint32_t lo = Rd32(s, a), hi = Rd32(s, a + 4);
						s.r[rt] = lo;
						s.r[(rt + 1) & 0xF] = hi;
					}
					else
						s.r[rt] = sz == 0 ? Rd32(s, a) : sz == 2 ? Rd8(s, a) : Rd16(s, a);
				}
				break;
			case KStrex:
				if ((ins & 0x0F900FF0) != 0x01800F90) goto GenericPath;
				{
					const uint32_t a = Rg(s, (ins >> 16) & 0xF, 0), rt = ins & 0xF, v = Rg(s, rt, 0), sz = (ins >> 21) & 3, rd = (ins >> 12) & 0xF;
					if (!c.iExcl) { s.r[rd] = 1; break; }
					c.iExcl = false;
					if (sz == 0) Wr32(s, a, v); else if (sz == 2) Wr8(s, a, v); else if (sz == 3) Wr16(s, a, v);
					else { Wr32(s, a, v); Wr32(s, a + 4, Rg(s, (rt + 1) & 0xF, 0)); }
					s.r[rd] = 0;
				}
				break;
			case KMovw:
				s.r[(ins >> 12) & 0xF] = ((ins >> 4) & 0xF000) | (ins & 0xFFF);
				break;
			case KMovt:
			{
				const uint32_t rd = (ins >> 12) & 0xF;
				s.r[rd] = (s.r[rd] & 0xFFFF) | ((((ins >> 4) & 0xF000) | (ins & 0xFFF)) << 16);
				break;
			}
			case KMedia:
				c.R[15] = s.np;
				c.Media(ins, s.cur);
				s.np = c.R[15];
				if (c.iHalt != ERunning) s.stop = true;
				break;
			default:
				goto GenericPath;
			}
			if (PH_UNLIKELY(s.stop)) break;
			continue;
			}
		GenericPath:
			// an encoding the fast classes only pre-select (the exact mask failed), or one this loop leaves to the generic code (SWI, BKPT,
			// undefined): undo the pre-increment and let Step() decide
			s.np = s.cur;
			--steps;
			c.R[15] = s.cur;
			Store(c, s.fl);
			c.iT = s.thumb ? 1 : 0;
			c.iSteps = steps;
			++c.iStatSlow;
			c.Step();
			steps = c.iSteps;
			s.np = c.R[15];
			s.fl = Load(c);
			s.thumb = c.iT != 0;
			if (c.iHalt != ERunning || c.iBpCount) { s.stop = c.iHalt != ERunning; break; }
			s.pmap = c.iPageMap;
			s.watch = c.iOnWatch != 0;
		}
		c.R[15] = s.np;
		Store(c, s.fl);
		c.iSteps = steps;
		c.iLastPc = s.cur;
		c.iT = s.thumb ? 1 : 0;
		return generic && !s.stop;
	}
};
uint8_t FastCore::sCls[4096];
uint16_t FastCore::sCond[16];
bool FastCore::sInit;

void CArmCpu::Run(uint64_t aMaxSteps)
{
	const uint64_t start = iSteps;
	while (iHalt == ERunning)
	{
		uint64_t limit = start + aMaxSteps;
		if (iSliceSteps && iNextSlice < limit) limit = iNextSlice;
		if (iOnProgress)
		{
			const uint64_t boundary = (iSteps | ((1ull << 26) - 1)) + 1;
			if (boundary < limit) limit = boundary;
		}
		if (limit <= iSteps) limit = iSteps + 1;				// always one instruction, as before
		if (!iBpCount)
		{
			if (FastCore::Batch(*this, limit) && iHalt == ERunning && iSteps < limit) Step();		// pc outside the region (trap, foreign pc), odd pc
		}
		else
		{
			do Step();
			while (iSteps < limit && iHalt == ERunning && iBpCount);
		}
		if (iOnProgress && !(iSteps & ((1u << 26) - 1))) iOnProgress(*this, iBpCtx);
		if (iSliceSteps && iSteps >= iNextSlice)
		{
			iNextSlice = iSteps + iSliceSteps;
			if (iOnSlice) iOnSlice(*this, iBpCtx);
			if (iHalt != ERunning) break;
		}
		if (iSteps - start >= aMaxSteps)
		{
			Halt(EStepLimit);
			break;
		}
	}
}
}

#endif

// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armcpu.cpp -- OURS (Proteus P0). See armcpu.h. ARM mode, user-mode instructions up to ARMv7-A: data processing (all 16 ops, barrel shifter), MUL/MLA/MLS/UMULL/UMLAL/
// UMAAL/SMULL/SMLAL, LDR/STR/LDRB/STRB, LDRH/STRH/LDRSB/LDRSH/LDRD/STRD, LDREX/STREX (all sizes), LDM/STM, B/BL/BX/BLX, CLZ, SWI, MRS/MSR (user fields), the ARMv5TE DSP
// group, MOVW/MOVT, the ARMv6/v7 media group (Media()), VFPv3. Not covered (stops with EUndefined): NEON, SWP. Thumb: thumb.cpp. R[15] is "the address of the NEXT
// instruction" while one executes, so reading PC = R[15] + 4 (+ 8 in a register-shifted operand).
#include "armcpu.h"
#include <string.h>

namespace proteus
{
CArmCpu::CArmCpu()
{
	memset(this, 0, sizeof(*this));
}

uint8_t* CArmCpu::HostPtr(uint32_t a) const
{
	if (InRegion(a, 1)) return iHost + (a - iBase);
	return (uint8_t*)(uintptr_t)a;			// a window (only on ILP32: guest address == host address)
}

bool CArmCpu::AddWindow(uint32_t aBase, uint32_t aSize)
{
	if (iWinCount >= KMaxWindows || sizeof(void*) != 4) return false;
	iWinBase[iWinCount] = aBase;
	iWinSize[iWinCount++] = aSize;
	return true;
}

void CArmCpu::Fault(uint32_t aAddr, const char* aWhat)
{
	if (iHalt == ERunning)
	{
		iHalt = EFault;
		iFaultAddr = aAddr;
		strncpy(iNote, aWhat, sizeof(iNote) - 1);
	}
}

uint32_t CArmCpu::Read32(uint32_t a)
{
	if (!Valid(a, 4)) { Fault(a, "read32"); return 0; }
	uint32_t v;
	memcpy(&v, HostPtr(a), 4);
	return v;
}
uint16_t CArmCpu::Read16(uint32_t a)
{
	if (!Valid(a, 2)) { Fault(a, "read16"); return 0; }
	uint16_t v;
	memcpy(&v, HostPtr(a), 2);
	return v;
}
uint8_t CArmCpu::Read8(uint32_t a)
{
	if (!Valid(a, 1)) { Fault(a, "read8"); return 0; }
	return *HostPtr(a);
}
void CArmCpu::Write32(uint32_t a, uint32_t v)
{
	if (iOnWatch && a < iWatchHi && a + 4 > iWatchLo) iOnWatch(*this, a, v, 4, iBpCtx);
	if (!Valid(a, 4)) { Fault(a, "write32"); return; }
	memcpy(HostPtr(a), &v, 4);
}
void CArmCpu::Write16(uint32_t a, uint16_t v)
{
	if (iOnWatch && a < iWatchHi && a + 2 > iWatchLo) iOnWatch(*this, a, v, 2, iBpCtx);
	if (!Valid(a, 2)) { Fault(a, "write16"); return; }
	memcpy(HostPtr(a), &v, 2);
}
void CArmCpu::Write8(uint32_t a, uint8_t v)
{
	if (iOnWatch && a < iWatchHi && a + 1 > iWatchLo) iOnWatch(*this, a, v, 1, iBpCtx);
	if (!Valid(a, 1)) { Fault(a, "write8"); return; }
	*HostPtr(a) = v;
}

const char* CArmCpu::CString(uint32_t a, char* buf, int max)
{
	int i = 0;
	for (; i < max - 1; ++i)
	{
		if (!Valid(a + i, 1)) break;
		char c = (char)*HostPtr(a + i);
		buf[i] = c;
		if (!c) return buf;
	}
	buf[i] = 0;
	return buf;
}

bool CArmCpu::Cond(uint32_t c) const
{
	switch (c)
	{
	case 0: return iZ;
	case 1: return !iZ;
	case 2: return iC;
	case 3: return !iC;
	case 4: return iN;
	case 5: return !iN;
	case 6: return iV;
	case 7: return !iV;
	case 8: return iC && !iZ;
	case 9: return !iC || iZ;
	case 10: return iN == iV;
	case 11: return iN != iV;
	case 12: return !iZ && iN == iV;
	case 13: return iZ || iN != iV;
	default: return true;
	}
}

void CArmCpu::Branch(uint32_t t)
{
	iT = t & 1;
	R[15] = t & ~1u;
}

// the shifted-register / rotated-immediate second operand; aCarry = shifter carry out
uint32_t CArmCpu::Operand2(uint32_t ins, bool& carry)
{
	carry = iC;
	if (ins & (1u << 25))
	{
		const uint32_t rot = ((ins >> 8) & 0xF) * 2;
		uint32_t v = ins & 0xFF;
		if (rot)
		{
			v = (v >> rot) | (v << (32 - rot));
			carry = (v >> 31) & 1;
		}
		return v;
	}
	const bool regShift = (ins & (1u << 4)) != 0;
	uint32_t rm = Reg(ins & 0xF, regShift ? 8 : 4);
	uint32_t amt = regShift ? (Reg((ins >> 8) & 0xF, 4) & 0xFF) : ((ins >> 7) & 0x1F);
	const uint32_t type = (ins >> 5) & 3;
	switch (type)
	{
	case 0:	// LSL
		if (amt == 0) return rm;
		if (amt < 32) { carry = (rm >> (32 - amt)) & 1; return rm << amt; }
		carry = amt == 32 ? (rm & 1) : 0;
		return 0;
	case 1:	// LSR
		if (!regShift && amt == 0) amt = 32;
		if (amt == 0) return rm;
		if (amt < 32) { carry = (rm >> (amt - 1)) & 1; return rm >> amt; }
		carry = amt == 32 ? (rm >> 31) & 1 : 0;
		return 0;
	case 2:	// ASR
		if (!regShift && amt == 0) amt = 32;
		if (amt == 0) return rm;
		if (amt < 32) { carry = (rm >> (amt - 1)) & 1; return (uint32_t)((int32_t)rm >> amt); }
		carry = (rm >> 31) & 1;
		return (rm >> 31) ? 0xFFFFFFFFu : 0;
	default:	// ROR / RRX
		if (!regShift && amt == 0)
		{
			const uint32_t r = (rm >> 1) | ((uint32_t)iC << 31);
			carry = rm & 1;
			return r;
		}
		if (amt == 0) return rm;
		amt &= 31;
		if (amt == 0) { carry = (rm >> 31) & 1; return rm; }
		carry = (rm >> (amt - 1)) & 1;
		return (rm >> amt) | (rm << (32 - amt));
	}
}

void CArmCpu::DataProc(uint32_t ins)
{
	const uint32_t op = (ins >> 21) & 0xF, rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
	const bool s = (ins & (1u << 20)) != 0;
	bool shc;
	const uint32_t b = Operand2(ins, shc);
	const uint32_t regShiftExtra = ((ins & (1u << 25)) == 0 && (ins & (1u << 4))) ? 8 : 4;
	const uint32_t a = Reg(rn, regShiftExtra);
	uint64_t res = 0;
	bool arith = false;
	bool carryIn = iC;
	bool write = true;
	uint32_t r = 0;
	switch (op)
	{
	case 0: r = a & b; break;						// AND
	case 1: r = a ^ b; break;						// EOR
	case 2: res = (uint64_t)a + (uint64_t)(~b) + 1; arith = true; r = (uint32_t)res; break;	// SUB
	case 3: res = (uint64_t)b + (uint64_t)(~a) + 1; arith = true; r = (uint32_t)res; break;	// RSB
	case 4: res = (uint64_t)a + b; arith = true; r = (uint32_t)res; break;					// ADD
	case 5: res = (uint64_t)a + b + carryIn; arith = true; r = (uint32_t)res; break;			// ADC
	case 6: res = (uint64_t)a + (uint64_t)(~b) + carryIn; arith = true; r = (uint32_t)res; break;	// SBC
	case 7: res = (uint64_t)b + (uint64_t)(~a) + carryIn; arith = true; r = (uint32_t)res; break;	// RSC
	case 8: r = a & b; write = false; break;		// TST
	case 9: r = a ^ b; write = false; break;		// TEQ
	case 10: res = (uint64_t)a + (uint64_t)(~b) + 1; arith = true; r = (uint32_t)res; write = false; break;	// CMP
	case 11: res = (uint64_t)a + b; arith = true; r = (uint32_t)res; write = false; break;					// CMN
	case 12: r = a | b; break;						// ORR
	case 13: r = b; break;							// MOV
	case 14: r = a & ~b; break;						// BIC
	default: r = ~b; break;							// MVN
	}
	if (s && !(rd == 15 && write))
	{
		iN = r >> 31;
		iZ = r == 0;
		if (arith)
		{
			iC = (res >> 32) & 1;
			uint32_t x = (op == 2 || op == 6 || op == 10) ? a : (op == 3 || op == 7) ? b : a;
			uint32_t y = (op == 2 || op == 6 || op == 10) ? ~b : (op == 3 || op == 7) ? ~a : b;
			iV = (~(x ^ y) & (x ^ r)) >> 31;
		}
		else
			iC = shc;
	}
	if (write)
	{
		if (rd == 15)
			Branch(r);
		else
			R[rd] = r;
	}
}

void CArmCpu::Multiply(uint32_t ins)
{
	const bool s = (ins & (1u << 20)) != 0;
	const uint32_t rm = R[ins & 0xF], rs = R[(ins >> 8) & 0xF];
	if (((ins >> 23) & 0x1F) == 0)	// MUL / MLA
	{
		const uint32_t rd = (ins >> 16) & 0xF, rn = (ins >> 12) & 0xF;
		uint32_t r = rm * rs;
		if (ins & (1u << 21))
			r += R[rn];
		R[rd] = r;
		if (s) { iN = r >> 31; iZ = r == 0; }
		return;
	}
	const uint32_t rdhi = (ins >> 16) & 0xF, rdlo = (ins >> 12) & 0xF;
	uint64_t r;
	if (ins & (1u << 22))
		r = (uint64_t)((int64_t)(int32_t)rm * (int64_t)(int32_t)rs);
	else
		r = (uint64_t)rm * (uint64_t)rs;
	if (ins & (1u << 21))
		r += ((uint64_t)R[rdhi] << 32) | R[rdlo];
	R[rdlo] = (uint32_t)r;
	R[rdhi] = (uint32_t)(r >> 32);
	if (s) { iN = (uint32_t)(r >> 63); iZ = r == 0; }
}

void CArmCpu::HalfwordTransfer(uint32_t ins)
{
	const bool p = ins & (1u << 24), u = ins & (1u << 23), w = ins & (1u << 21), l = ins & (1u << 20);
	const uint32_t rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
	const uint32_t sh = (ins >> 5) & 3;
	const uint32_t off = (ins & (1u << 22)) ? (((ins >> 4) & 0xF0) | (ins & 0xF)) : R[ins & 0xF];
	uint32_t base = Reg(rn, 4);
	uint32_t addr = p ? (u ? base + off : base - off) : base;
	if (l)
	{
		if (sh == 1) R[rd] = Read16(addr);
		else if (sh == 2) R[rd] = (uint32_t)(int32_t)(int8_t)Read8(addr);
		else if (sh == 3) R[rd] = (uint32_t)(int32_t)(int16_t)Read16(addr);
	}
	else
	{
		if (sh == 1) Write16(addr, (uint16_t)Reg(rd, 8));
		else if (sh == 2) { R[rd] = Read32(addr); R[rd + 1] = Read32(addr + 4); }		// LDRD
		else if (sh == 3) { Write32(addr, R[rd]); Write32(addr + 4, R[rd + 1]); }		// STRD
	}
	if (!p || w)
		R[rn] = p ? addr : (u ? base + off : base - off);
}

void CArmCpu::SingleTransfer(uint32_t ins)
{
	const bool reg = ins & (1u << 25), p = ins & (1u << 24), u = ins & (1u << 23), bt = ins & (1u << 22), w = ins & (1u << 21), l = ins & (1u << 20);
	const uint32_t rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF;
	uint32_t off;
	if (!reg)
		off = ins & 0xFFF;
	else
	{
		bool c;
		off = Operand2(ins & ~(1u << 25) & ~(1u << 4), c);	// immediate-shifted register
	}
	const uint32_t base = Reg(rn, 4);
	const uint32_t addr = p ? (u ? base + off : base - off) : base;
	if (l)
	{
		uint32_t v;
		if (bt)
			v = Read8(addr);
		else
		{
			v = Read32(addr & ~3u);
			const uint32_t rot = 8 * (addr & 3);
			if (rot) v = (v >> rot) | (v << (32 - rot));
		}
		if (!p || w) R[rn] = p ? addr : (u ? base + off : base - off);
		if (rd == 15) Branch(v);
		else R[rd] = v;
	}
	else
	{
		const uint32_t v = Reg(rd, 8);
		if (bt) Write8(addr, (uint8_t)v);
		else Write32(addr, v);
		if (!p || w) R[rn] = p ? addr : (u ? base + off : base - off);
	}
}

void CArmCpu::BlockTransfer(uint32_t ins)
{
	const bool p = ins & (1u << 24), u = ins & (1u << 23), w = ins & (1u << 21), l = ins & (1u << 20);
	const uint32_t rn = (ins >> 16) & 0xF, list = ins & 0xFFFF;
	int n = 0;
	for (int i = 0; i < 16; ++i)
		n += (list >> i) & 1;
	const uint32_t base = R[rn];
	uint32_t addr = u ? (p ? base + 4 : base) : (p ? base - 4 * n : base - 4 * n + 4);
	const uint32_t wb = u ? base + 4 * n : base - 4 * n;
	for (int i = 0; i < 16; ++i)
	{
		if (!((list >> i) & 1)) continue;
		if (l)
		{
			const uint32_t v = Read32(addr);
			if (i == 15) Branch(v);
			else R[i] = v;
		}
		else
			Write32(addr, i == 15 ? R[15] + 8 : R[i]);
		addr += 4;
	}
	if (w && !(l && ((list >> rn) & 1)))
		R[rn] = wb;
}

void CArmCpu::Step()
{
	const uint32_t pc = R[15];
	iLastPc = pc;
	for (int b = 0; b < iBpCount; ++b) if (iBp[b] == pc && iOnBp) iOnBp(*this, b, iBpCtx);
	if (pc >= KTrapBase && pc < KTrapEnd)
	{
		if (iTrap) iTrap(*this, pc, iCtx);
		else Halt(EHleStop);
		return;
	}
	if (pc == KReturnToHost)
	{
		Halt(EReturned, (int32_t)R[0]);
		return;
	}
	if (iOnForeignPc && pc - iBase > iSize - 2)		// outside the guest region: native code (a native object's vtable) -> its guest twin?
	{
		const uint32_t t = iOnForeignPc(*this, pc, iBpCtx);
		if (t)
		{
			Branch(t);
			return;
		}
	}
	if (iT)
	{
		ThumbStep(pc);
		return;
	}
	// instruction fetch: the guest region directly (the profile's 3rd hot spot was Read32 = Valid + window scan per instruction)
	uint32_t ins;
	if (pc - iBase <= iSize - 4) memcpy(&ins, iHost + (pc - iBase), 4);
	else ins = Read32(pc);
	if (iHalt != ERunning) return;
	R[15] = pc + 4;
	++iSteps;
	const uint32_t cond = ins >> 28;
	if (cond == 0xF)
	{
		if (((ins >> 25) & 7) == 5)		// BLX immediate
		{
			int32_t off = (int32_t)(ins << 8) >> 6;
			R[14] = pc + 4;
			R[15] = pc + 8 + off + ((ins >> 24) & 1) * 2;
			Branch(R[15] | 1);			// always to Thumb
			return;
		}
		if (((ins >> 25) & 7) == 1 || (ins & 0x0F100000) == 0x04000000)	// NEON data processing / element and structure load-store
		{
			Halt(EUndefined);
			iFaultAddr = pc;
			strncpy(iNote, "NEON not implemented", sizeof(iNote) - 1);
			return;
		}
		return;							// PLD/PLI, DMB/DSB/ISB, CLREX, SETEND, CPS: no-op for one user-mode guest thread
	}
	if (!Cond(cond))
		return;
	switch ((ins >> 25) & 7)
	{
	case 0:
		if ((ins & 0x0FFFFFF0) == 0x012FFF10) { Branch(R[ins & 0xF]); return; }					// BX
		if ((ins & 0x0FFFFFF0) == 0x012FFF30) { const uint32_t t = R[ins & 0xF]; R[14] = pc + 4; Branch(t); return; }	// BLX reg
		if ((ins & 0x0FFF0FF0) == 0x016F0F10) { uint32_t v = R[ins & 0xF], n = 0; if (!v) n = 32; else while (!(v & 0x80000000u)) { v <<= 1; ++n; } R[(ins >> 12) & 0xF] = n; return; }	// CLZ
		if ((ins & 0x0FF000F0) == 0x00600090)		// MLS (ARMv6T2): Rd = Ra - Rn * Rm
		{
			R[(ins >> 16) & 0xF] = R[(ins >> 12) & 0xF] - R[ins & 0xF] * R[(ins >> 8) & 0xF];
			return;
		}
		if ((ins & 0x0FF000F0) == 0x00400090)		// UMAAL (ARMv6): RdHi:RdLo = Rn * Rm + RdHi + RdLo
		{
			const uint32_t hi = (ins >> 16) & 0xF, lo = (ins >> 12) & 0xF;
			const uint64_t v = (uint64_t)R[ins & 0xF] * R[(ins >> 8) & 0xF] + R[hi] + R[lo];
			R[lo] = (uint32_t)v;
			R[hi] = (uint32_t)(v >> 32);
			return;
		}
		if ((ins & 0x0FC000F0) == 0x00000090 || (ins & 0x0F8000F0) == 0x00800090) { Multiply(ins); return; }
		if ((ins & 0x0E000090) == 0x00000090 && (ins & 0x60) != 0) { HalfwordTransfer(ins); return; }
		// LDREX/STREX (ARMv6, word/byte/half: the real euser's atomics): one guest thread, so the exclusive monitor always succeeds
		if ((ins & 0x0F900FFF) == 0x01900F9F)
		{
			const uint32_t a = R[(ins >> 16) & 0xF], sz = (ins >> 21) & 3, rt = (ins >> 12) & 0xF;
			if (sz == 1)				// LDREXD Rt, Rt+1
			{
				const uint32_t lo = Read32(a), hi = Read32(a + 4);
				R[rt] = lo;
				R[(rt + 1) & 0xF] = hi;
				return;
			}
			R[rt] = sz == 0 ? Read32(a) : sz == 2 ? Read8(a) : Read16(a);
			return;
		}
		if ((ins & 0x0F900FF0) == 0x01800F90)
		{
			const uint32_t a = R[(ins >> 16) & 0xF], rt = ins & 0xF, v = R[rt], sz = (ins >> 21) & 3;
			if (sz == 0) Write32(a, v); else if (sz == 2) Write8(a, (uint8_t)v); else if (sz == 3) Write16(a, (uint16_t)v);
			else { Write32(a, v); Write32(a + 4, R[(rt + 1) & 0xF]); }		// STREXD Rt, Rt+1
			R[(ins >> 12) & 0xF] = 0;
			return;
		}
		// ARMv5TE DSP: SMLA<x><y> / SMLAW<y> / SMULW<y> / SMLAL<x><y> / SMUL<x><y> (Q flag not modelled)
		if ((ins & 0x0F900090) == 0x01000080)
		{
			const uint32_t op = (ins >> 21) & 3, rd = (ins >> 16) & 0xF, rn = (ins >> 12) & 0xF, rs = (ins >> 8) & 0xF, rm = ins & 0xF;
			const int32_t mx = (ins & 0x20) ? (int32_t)R[rm] >> 16 : (int32_t)(int16_t)R[rm];
			const int32_t sy = (ins & 0x40) ? (int32_t)R[rs] >> 16 : (int32_t)(int16_t)R[rs];
			switch (op)
			{
			case 0: R[rd] = (uint32_t)(mx * sy) + R[rn]; break;
			case 1:
			{
				const int32_t w = (int32_t)(((int64_t)(int32_t)R[rm] * sy) >> 16);
				R[rd] = (ins & 0x20) ? (uint32_t)w : (uint32_t)w + R[rn];
				break;
			}
			case 2:
			{
				int64_t acc = (int64_t)(((uint64_t)R[rd] << 32) | R[rn]);
				acc += (int64_t)mx * sy;
				R[rn] = (uint32_t)acc;
				R[rd] = (uint32_t)((uint64_t)acc >> 32);
				break;
			}
			default: R[rd] = (uint32_t)(mx * sy); break;
			}
			return;
		}
		if ((ins & 0x0F900FF0) == 0x01000050)		// QADD / QSUB / QDADD / QDSUB (saturating)
		{
			const uint32_t op = (ins >> 21) & 3, rd = (ins >> 12) & 0xF;
			const int64_t rm = (int32_t)R[ins & 0xF];
			int64_t rn = (int32_t)R[(ins >> 16) & 0xF];
			if (op & 2) { rn *= 2; if (rn > 0x7FFFFFFF) rn = 0x7FFFFFFF; if (rn < -0x7FFFFFFFLL - 1) rn = -0x7FFFFFFFLL - 1; }
			int64_t v = (op & 1) ? rm - rn : rm + rn;
			if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
			if (v < -0x7FFFFFFFLL - 1) v = -0x7FFFFFFFLL - 1;
			R[rd] = (uint32_t)v;
			return;
		}
		if ((ins & 0x0FBF0FFF) == 0x010F0000)		// MRS Rd, CPSR (P2: user mode 0x10 + the flags; SPSR reads the same)
		{
			R[(ins >> 12) & 0xF] = (iN << 31) | (iZ << 30) | (iC << 29) | (iV << 28) | (iGE << 16) | (iT << 5) | 0x10;
			return;
		}
		if ((ins & 0x0FB0FFF0) == 0x0120F000)		// MSR CPSR_<fields>, Rm: user mode can only change the flags (field f) and GE (field s)
		{
			const uint32_t v = R[ins & 0xF];
			if (ins & (1u << 19)) { iN = v >> 31; iZ = (v >> 30) & 1; iC = (v >> 29) & 1; iV = (v >> 28) & 1; }
			if (ins & (1u << 18)) iGE = (v >> 16) & 0xF;
			return;
		}
		if ((ins & 0x01900000) == 0x01000000)		// TST..CMN without S = the misc group (MRS/MSR/BKPT/saturating/DSP multiplies)
		{
			Halt(EUndefined);
			iFaultAddr = pc;
			strncpy(iNote, "misc instruction group (MRS/MSR/QADD/DSP) not implemented", sizeof(iNote) - 1);
			return;
		}
		DataProc(ins);
		return;
	case 1:
		if ((ins & 0x0FB0F000) == 0x0320F000)		// MSR CPSR_<fields>, #imm (flags only in user mode; field mask 0 = NOP/hints)
		{
			const uint32_t rot = ((ins >> 8) & 0xF) * 2, imm = ins & 0xFF;
			const uint32_t v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
			if (ins & (1u << 19)) { iN = v >> 31; iZ = (v >> 30) & 1; iC = (v >> 29) & 1; iV = (v >> 28) & 1; }
			if (ins & (1u << 18)) iGE = (v >> 16) & 0xF;
			return;
		}
		if ((ins & 0x0FF00000) == 0x03000000)		// MOVW (ARMv6T2): Rd = imm16
		{
			R[(ins >> 12) & 0xF] = ((ins >> 4) & 0xF000) | (ins & 0xFFF);
			return;
		}
		if ((ins & 0x0FF00000) == 0x03400000)		// MOVT (ARMv6T2): Rd[31:16] = imm16
		{
			const uint32_t rd = (ins >> 12) & 0xF;
			R[rd] = (R[rd] & 0xFFFF) | ((((ins >> 4) & 0xF000) | (ins & 0xFFF)) << 16);
			return;
		}
		if ((ins & 0x01900000) == 0x01000000) { Halt(EUndefined); iFaultAddr = pc; strncpy(iNote, "MSR immediate not implemented", sizeof(iNote) - 1); return; }
		DataProc(ins);
		return;
	case 2:
	case 3:
		if ((ins & 0x0E000010) == 0x06000010) { Media(ins, pc); return; }
		SingleTransfer(ins);
		return;
	case 4:
		BlockTransfer(ins);
		return;
	case 6:
		VfpTransfer(ins, pc);
		return;
	case 5:
	{
		const int32_t off = (int32_t)(ins << 8) >> 6;
		if (ins & (1u << 24)) R[14] = pc + 4;
		Branch(pc + 8 + off);
		return;
	}
	case 7:
		if (ins & (1u << 24))
		{
			if (!iSvc || !iSvc(*this, ins & 0xFFFFFF, iCtx))
			{
				Halt(ESvcUnhandled, (int32_t)(ins & 0xFFFFFF));
				iFaultAddr = pc;
			}
			return;
		}
		VfpOp(ins, pc);
		return;
	default:
		Halt(EUndefined);
		iFaultAddr = pc;
		strncpy(iNote, "coprocessor / VFP instruction (needs the dynarmic phase)", sizeof(iNote) - 1);
		return;
	}
}

// ---- the ARMv6/v7 media group (cond 011x xxxx ... bit 4 = 1; ARM ARM "media instructions"): op1 = bits 24:20, op2 = bits 7:5 --------------------------
static inline uint32_t Ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline int32_t SatS(int64_t v, uint32_t bits) { const int64_t hi = (1LL << (bits - 1)) - 1, lo = -(1LL << (bits - 1)); return (int32_t)(v > hi ? hi : v < lo ? lo : v); }
static inline uint32_t SatU(int64_t v, uint32_t bits) { const int64_t hi = (int64_t)((1ULL << bits) - 1); return (uint32_t)(v > hi ? hi : v < 0 ? 0 : v); }

// SADD16..UHSUB8: prefix (op1 bits 22:20) S=1 Q=2 SH=3 U=5 UQ=6 UH=7; op (op2) ADD16=0 ASX=1 SAX=2 SUB16=3 ADD8=4 SUB8=7. S and U set GE.
void CArmCpu::ParallelAddSub(uint32_t ins, uint32_t pc)
{
	const uint32_t pre = (ins >> 20) & 7, op = (ins >> 5) & 7, n = R[(ins >> 16) & 0xF], m = R[ins & 0xF];
	if (pre == 0 || pre == 4 || op == 5 || op == 6) { Halt(EUndefined); iFaultAddr = pc; strncpy(iNote, "undefined parallel add/subtract", sizeof(iNote) - 1); return; }
	const bool sgn = pre < 4, b8 = op >= 4;
	const uint32_t w = b8 ? 8 : 16, lanes = b8 ? 4 : 2, mask = (1u << w) - 1;
	uint32_t res = 0, ge = 0;
	for (uint32_t i = 0; i < lanes; ++i)
	{
		bool sub;							// this lane subtracts
		uint32_t mi = i;					// the lane of Rm it uses (ASX/SAX cross the halves)
		if (op == 1) { sub = i == 0; mi = 1 - i; }
		else if (op == 2) { sub = i == 1; mi = 1 - i; }
		else sub = op == 3 || op == 7;
		uint32_t ua = (n >> (i * w)) & mask, ub = (m >> (mi * w)) & mask;
		int64_t a = ua, b = ub;
		if (sgn) { a = (int64_t)((int32_t)(ua << (32 - w)) >> (32 - w)); b = (int64_t)((int32_t)(ub << (32 - w)) >> (32 - w)); }
		int64_t r = sub ? a - b : a + b;
		bool g = false;
		switch (pre & 3)
		{
		case 1: g = sgn ? r >= 0 : (sub ? r >= 0 : r >= (int64_t)1 << w); break;		// S / U: modular, GE
		case 2: r = sgn ? SatS(r, w) : SatU(r, w); break;							// Q / UQ: saturating
		case 3: r >>= 1; break;													// SH / UH: halving
		}
		res |= ((uint32_t)r & mask) << (i * w);
		if (g) ge |= b8 ? 1u << i : 3u << (i * 2);
	}
	R[(ins >> 12) & 0xF] = res;
	if ((pre & 3) == 1) iGE = ge;
}

void CArmCpu::Media(uint32_t ins, uint32_t pc)
{
	const uint32_t op1 = (ins >> 20) & 0x1F, op2 = (ins >> 5) & 7;
	const uint32_t rn = (ins >> 16) & 0xF, rd = (ins >> 12) & 0xF, rs = (ins >> 8) & 0xF, rm = ins & 0xF;
	if (op1 < 8) { ParallelAddSub(ins, pc); return; }
	if (op1 < 16)							// packing, unpacking, saturation, reversal (Rd 15:12, Rn 19:16, Rm 3:0)
	{
		const uint32_t rot = ((ins >> 10) & 3) * 8, imm5 = (ins >> 7) & 0x1F;
		if (op1 == 8 && !(op2 & 1))			// PKHBT / PKHTB
		{
			if (op2 & 2) R[rd] = (R[rn] & 0xFFFF0000) | ((uint32_t)((int32_t)R[rm] >> (imm5 ? imm5 : 31)) & 0xFFFF);
			else R[rd] = (R[rn] & 0xFFFF) | ((R[rm] << imm5) & 0xFFFF0000);
			return;
		}
		if (op1 == 8 && op2 == 5)			// SEL: each byte from Rn where GE[i], else Rm
		{
			uint32_t r = 0;
			for (int i = 0; i < 4; ++i) r |= (((iGE >> i) & 1) ? R[rn] : R[rm]) & (0xFFu << (i * 8));
			R[rd] = r;
			return;
		}
		if ((op1 & 0x1A) == 0x0A && !(op2 & 1))	// SSAT / USAT: Rd = sat(shift(Rn at 3:0)), sat_imm 20:16
		{
			const uint32_t sat = (ins >> 16) & 0x1F, v = R[rm];
			const int64_t x = (ins & 0x40) ? (int64_t)((int32_t)v >> (imm5 ? imm5 : 31)) : (int64_t)(int32_t)(v << imm5);
			R[rd] = (op1 & 4) ? SatU(x, sat) : (uint32_t)SatS(x, sat + 1);
			return;
		}
		if ((op1 == 0x0A || op1 == 0x0E) && op2 == 1)	// SSAT16 / USAT16: sat_imm 19:16
		{
			const uint32_t sat = (ins >> 16) & 0xF, v = R[rm];
			const int64_t lo = (int16_t)v, hi = (int16_t)(v >> 16);
			R[rd] = op1 == 0x0E ? (SatU(lo, sat) & 0xFFFF) | (SatU(hi, sat) << 16)
			                    : ((uint32_t)SatS(lo, sat + 1) & 0xFFFF) | ((uint32_t)SatS(hi, sat + 1) << 16);
			return;
		}
		if (op2 == 3)						// the extends, rotated by 0/8/16/24; Rn != 15 adds (the -A forms)
		{
			const uint32_t v = Ror(R[rm], rot), add = rn == 15 ? 0 : R[rn];
			uint32_t r;
			switch (op1 & 7)
			{
			case 0: { const uint32_t lo = (uint32_t)(int32_t)(int8_t)v + add, hi = (uint32_t)(int32_t)(int8_t)(v >> 16) + (add >> 16); r = (lo & 0xFFFF) | (hi << 16); break; }	// SXTB16
			case 2: r = (uint32_t)(int32_t)(int8_t)v + add; break;			// SXTB
			case 3: r = (uint32_t)(int32_t)(int16_t)v + add; break;			// SXTH
			case 4: { const uint32_t lo = (v & 0xFF) + add, hi = ((v >> 16) & 0xFF) + (add >> 16); r = (lo & 0xFFFF) | (hi << 16); break; }	// UXTB16
			case 6: r = (v & 0xFF) + add; break;								// UXTB
			case 7: r = (v & 0xFFFF) + add; break;							// UXTH
			default: Halt(EUndefined); iFaultAddr = pc; strncpy(iNote, "undefined extend", sizeof(iNote) - 1); return;
			}
			R[rd] = r;
			return;
		}
		const uint32_t v = R[rm];
		if (op1 == 0x0B && op2 == 1) { R[rd] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); return; }		// REV
		if (op1 == 0x0B && op2 == 5) { R[rd] = ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00); return; }							// REV16
		if (op1 == 0x0F && op2 == 5) { R[rd] = (uint32_t)(int32_t)(int16_t)(((v >> 8) & 0xFF) | (v << 8)); return; }				// REVSH
		if (op1 == 0x0F && op2 == 1)		// RBIT
		{
			uint32_t r = 0;
			for (int i = 0; i < 32; ++i) r |= ((v >> i) & 1) << (31 - i);
			R[rd] = r;
			return;
		}
	}
	else if (op1 < 24)						// signed multiplies + divide (Rd 19:16, Ra 15:12, Rm 11:8, Rn 3:0)
	{
		const uint32_t n = R[rm], m0 = R[rs], m = (op2 & 1) ? Ror(m0, 16) : m0;	// rm field (3:0) = Rn here, rs (11:8) = Rm; bit 5 = swap Rm's halves
		const int64_t p1 = (int64_t)(int16_t)n * (int16_t)m, p2 = (int64_t)(int16_t)(n >> 16) * (int16_t)(m >> 16);
		if (op1 == 0x10 && op2 < 4)			// SMLAD/SMUAD (op2 0x), SMLSD/SMUSD (op2 1x)
		{
			const int64_t s = (op2 & 2) ? p1 - p2 : p1 + p2;
			R[rn] = (uint32_t)s + (rd == 15 ? 0 : R[rd]);
			return;
		}
		if ((op1 == 0x11 || op1 == 0x13) && op2 == 0)	// SDIV / UDIV (Rd = Rn / Rm; x / 0 = 0, INT_MIN / -1 = INT_MIN)
		{
			const uint32_t a = R[rm], b = R[rs];
			if (!b) R[rn] = 0;
			else if (op1 == 0x13) R[rn] = a / b;
			else R[rn] = (a == 0x80000000u && b == 0xFFFFFFFFu) ? a : (uint32_t)((int32_t)a / (int32_t)b);
			return;
		}
		if (op1 == 0x14 && op2 < 4)			// SMLALD / SMLSLD: RdHi (19:16) : RdLo (15:12) += p1 +/- p2
		{
			int64_t acc = (int64_t)(((uint64_t)R[rn] << 32) | R[rd]);
			acc += (op2 & 2) ? p1 - p2 : p1 + p2;
			R[rd] = (uint32_t)acc;
			R[rn] = (uint32_t)((uint64_t)acc >> 32);
			return;
		}
		if (op1 == 0x15 && (op2 < 2 || op2 >= 6))	// SMMLA/SMMUL (op2 00x), SMMLS (11x); bit 5 = round
		{
			const int64_t prod = (int64_t)(int32_t)R[rm] * (int32_t)R[rs];
			int64_t r = (int64_t)((uint64_t)(rd == 15 && op2 < 2 ? 0 : R[rd]) << 32);
			r = op2 < 2 ? r + prod : r - prod;
			if (ins & 0x20) r += 0x80000000LL;
			R[rn] = (uint32_t)((uint64_t)r >> 32);
			return;
		}
	}
	else
	{
		if (op1 == 0x18 && op2 == 0)		// USAD8 / USADA8 (Ra 15:12 = 15: no accumulate)
		{
			const uint32_t a = R[rm], b = R[rs];
			uint32_t s = 0;
			for (int i = 0; i < 4; ++i) { const int d = (int)((a >> (i * 8)) & 0xFF) - (int)((b >> (i * 8)) & 0xFF); s += d < 0 ? -d : d; }
			R[rn] = s + (rd == 15 ? 0 : R[rd]);
			return;
		}
		const uint32_t lsb = (ins >> 7) & 0x1F, hi = (ins >> 16) & 0x1F;
		if ((op1 & 0x1E) == 0x1C && !(op2 & 3))		// BFC (Rn = 15) / BFI: msb 20:16
		{
			if (hi >= lsb)
			{
				const uint32_t mask = (uint32_t)(((1ULL << (hi - lsb + 1)) - 1) << lsb);
				R[rd] = (R[rd] & ~mask) | ((rm == 15 ? 0 : R[rm] << lsb) & mask);
			}
			return;
		}
		if (((op1 & 0x1E) == 0x1A || (op1 & 0x1E) == 0x1E) && (op2 & 3) == 2)	// SBFX / UBFX: widthm1 20:16
		{
			const uint32_t width = hi + 1;
			if (lsb + width > 32) { Halt(EUndefined); iFaultAddr = pc; strncpy(iNote, "bitfield past bit 31", sizeof(iNote) - 1); return; }
			const uint32_t v = (uint32_t)((R[rm] >> lsb) & ((1ULL << width) - 1));
			R[rd] = (op1 & 0x1E) == 0x1A && width < 32 ? (uint32_t)((int32_t)(v << (32 - width)) >> (32 - width)) : v;
			return;
		}
	}
	Halt(EUndefined);
	iFaultAddr = pc;
	strncpy(iNote, "media instruction not implemented", sizeof(iNote) - 1);
}

// ---- VFP (rung 30: calcsoft.exe stopped at its first VPUSH in the OpenVG reference rasteriser) --------------------------------------
// ARM-mode VFPv3 (single + double): VLDR/VSTR, VLDM/VSTM (VPUSH/VPOP), VMOV (core <-> S, core pair <-> D/S pair, scalar word, immediate, reg),
// VADD/VSUB/VMUL/VDIV/VMLA/VMLS/VNMLA/VNMLS/VNMUL, VABS/VNEG/VSQRT, VCMP(E), VCVT (S<->D, int<->float), VMRS/VMSR. Host IEEE arithmetic, round to
// nearest; no flush-to-zero, no exceptions, no NEON, no Thumb-2 encodings (thumb.cpp is Thumb-1).
static inline float F32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t U32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double F64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t U64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

uint32_t CArmCpu::GetS(int n) const { return (n & 1) ? (uint32_t)(iD[(n >> 1) & 31] >> 32) : (uint32_t)iD[(n >> 1) & 31]; }
void CArmCpu::SetS(int n, uint32_t v)
{
	uint64_t& d = iD[(n >> 1) & 31];
	d = (n & 1) ? ((d & 0xFFFFFFFFull) | ((uint64_t)v << 32)) : ((d & 0xFFFFFFFF00000000ull) | v);
}

void CArmCpu::VfpUndefined(uint32_t pc, uint32_t ins)
{
	Halt(EUndefined);
	iFaultAddr = pc;
	strncpy(iNote, "VFP/coprocessor encoding not implemented", sizeof(iNote) - 1);
	(void)ins;
}

// ins >> 25 & 7 == 6: coprocessor load/store + 64-bit transfers
void CArmCpu::VfpTransfer(uint32_t ins, uint32_t pc)
{
	const uint32_t cp = (ins >> 8) & 0xF;
	if (cp != 10 && cp != 11) { VfpUndefined(pc, ins); return; }
	const bool dbl = cp == 11;
	const uint32_t rn = (ins >> 16) & 0xF, vd = (ins >> 12) & 0xF;
	if ((ins & 0x0FE00000) == 0x0C400000)			// VMOV Rt, Rt2 <-> D / S pair
	{
		const uint32_t rt = (ins >> 12) & 0xF, rt2 = (ins >> 16) & 0xF, vm = ins & 0xF, m = (ins >> 5) & 1;
		if (dbl)
		{
			uint64_t& d = iD[(m << 4) | vm];
			if (ins & (1u << 20)) { R[rt] = (uint32_t)d; R[rt2] = (uint32_t)(d >> 32); }
			else d = (uint64_t)R[rt] | ((uint64_t)R[rt2] << 32);
		}
		else
		{
			const int s = (vm << 1) | m;
			if (ins & (1u << 20)) { R[rt] = GetS(s); R[rt2] = GetS(s + 1); }
			else { SetS(s, R[rt]); SetS(s + 1, R[rt2]); }
		}
		return;
	}
	const bool P = ins & (1u << 24), U = ins & (1u << 23), W = ins & (1u << 21), L = ins & (1u << 20);
	const uint32_t imm = (ins & 0xFF) * 4, D = (ins >> 22) & 1;
	uint32_t base = rn == 15 ? ((pc + 8) & ~3u) : R[rn];
	if (P && !W)					// VLDR / VSTR
	{
		const uint32_t a = U ? base + imm : base - imm;
		if (dbl)
		{
			uint64_t& d = iD[(D << 4) | vd];
			if (L) d = (uint64_t)Read32(a) | ((uint64_t)Read32(a + 4) << 32);
			else { Write32(a, (uint32_t)d); Write32(a + 4, (uint32_t)(d >> 32)); }
		}
		else
		{
			const int s = (vd << 1) | D;
			if (L) SetS(s, Read32(a)); else Write32(a, GetS(s));
		}
		return;
	}
	// VLDM / VSTM / VPUSH / VPOP: IA (P=0,U=1) or DB (P=1,U=0), optional writeback
	if (!((!P && U) || (P && !U))) { VfpUndefined(pc, ins); return; }
	uint32_t a = P ? base - imm : base;
	const uint32_t wb = P ? a : base + imm;
	const uint32_t words = imm / 4;
	if (dbl)
	{
		const uint32_t first = (D << 4) | vd, n = words / 2;
		for (uint32_t i = 0; i < n; ++i, a += 8)
		{
			uint64_t& d = iD[(first + i) & 31];
			if (L) d = (uint64_t)Read32(a) | ((uint64_t)Read32(a + 4) << 32);
			else { Write32(a, (uint32_t)d); Write32(a + 4, (uint32_t)(d >> 32)); }
		}
	}
	else
	{
		const int first = (vd << 1) | D;
		for (uint32_t i = 0; i < words; ++i, a += 4)
		{
			if (L) SetS(first + i, Read32(a)); else Write32(a, GetS(first + i));
		}
	}
	if (W) R[rn] = wb;
}

static uint32_t VfpImm32(uint32_t imm8)
{
	const uint32_t b6 = (imm8 >> 6) & 1;
	return ((imm8 >> 7) << 31) | ((b6 ^ 1) << 30) | ((b6 ? 0x1Fu : 0) << 25) | (((imm8 >> 4) & 3) << 23) | ((imm8 & 0xF) << 19);
}
static uint64_t VfpImm64(uint32_t imm8)
{
	const uint64_t b6 = (imm8 >> 6) & 1;
	return ((uint64_t)(imm8 >> 7) << 63) | ((b6 ^ 1) << 62) | ((b6 ? 0xFFull : 0) << 54) | ((uint64_t)((imm8 >> 4) & 3) << 52) | ((uint64_t)(imm8 & 0xF) << 48);
}

template<typename T> static void VfpCompare(uint32_t& fpscr, T a, T b)
{
	uint32_t f;
	if (a != a || b != b) f = 0x3;				// unordered: C V
	else if (a == b) f = 0x6;					// Z C
	else if (a < b) f = 0x8;					// N
	else f = 0x2;								// C
	fpscr = (fpscr & 0x0FFFFFFFu) | (f << 28);
}

// ins >> 25 & 7 == 7 with bit 24 clear: coprocessor data processing + register transfers
void CArmCpu::VfpOp(uint32_t ins, uint32_t pc)
{
	const uint32_t cp = (ins >> 8) & 0xF;
	// MRC/MCR p15, 0, Rt, c13, c0, 2/3: the user thread ID registers (TPIDRURW read/write, TPIDRURO read-only); the rest of CP15 is privileged
	if ((ins & 0x00EF0FDF) == 0x000D0F50)
	{
		const uint32_t rt = (ins >> 12) & 0xF, opc2 = (ins >> 5) & 7;
		if (ins & (1u << 20)) { R[rt] = opc2 == 2 ? iTpidrurw : iTpidruro; return; }
		if (opc2 == 2) { iTpidrurw = R[rt]; return; }
	}
	if (cp != 10 && cp != 11) { VfpUndefined(pc, ins); return; }
	const bool dbl = cp == 11;
	const uint32_t vd = (ins >> 12) & 0xF, vn = (ins >> 16) & 0xF, vm = ins & 0xF;
	const uint32_t D = (ins >> 22) & 1, N = (ins >> 7) & 1, M = (ins >> 5) & 1;
	if (ins & 0x10)								// register transfer (MRC / MCR)
	{
		const uint32_t rt = (ins >> 12) & 0xF, opc1 = (ins >> 21) & 7;
		const bool L = ins & (1u << 20);
		if (opc1 == 7)							// VMRS / VMSR
		{
			if (L)
			{
				const uint32_t v = vn == 1 ? iFpscr : vn == 8 ? iFpexc : vn == 0 ? 0x41023030u : 0;
				if (rt == 15) { iN = v >> 31; iZ = (v >> 30) & 1; iC = (v >> 29) & 1; iV = (v >> 28) & 1; }
				else R[rt] = v;
			}
			else if (vn == 1) iFpscr = R[rt]; else if (vn == 8) iFpexc = R[rt];
			return;
		}
		if (!dbl && opc1 == 0)					// VMOV Rt <-> Sn
		{
			const int s = (vn << 1) | N;
			if (L) R[rt] = GetS(s); else SetS(s, R[rt]);
			return;
		}
		if (dbl && (opc1 & 5) == 0 && (ins & 0x60) == 0)	// VMOV Rt <-> Dn[H] (32-bit scalar)
		{
			uint64_t& d = iD[(N << 4) | vn];
			const bool hi = opc1 & 2;
			if (L) R[rt] = hi ? (uint32_t)(d >> 32) : (uint32_t)d;
			else d = hi ? ((d & 0xFFFFFFFFull) | ((uint64_t)R[rt] << 32)) : ((d & 0xFFFFFFFF00000000ull) | R[rt]);
			return;
		}
		VfpUndefined(pc, ins);
		return;
	}
	// data processing
	const int d = dbl ? (int)((D << 4) | vd) : (int)((vd << 1) | D);
	const int n = dbl ? (int)((N << 4) | vn) : (int)((vn << 1) | N);
	const int m = dbl ? (int)((M << 4) | vm) : (int)((vm << 1) | M);
	const uint32_t p = (ins >> 23) & 1, q = (ins >> 21) & 1, r = (ins >> 20) & 1, s = (ins >> 6) & 1;
	if (!(p && q && r))
	{
		const uint32_t op = (p << 3) | (q << 2) | (r << 1) | s;
		if (op == 8)							// VDIV
		{
			if (dbl) iD[d] = U64(F64(iD[n]) / F64(iD[m])); else SetS(d, U32(F32(GetS(n)) / F32(GetS(m))));
			return;
		}
		if (p) { VfpUndefined(pc, ins); return; }
		if (dbl)
		{
			const double a = F64(iD[n]), b = F64(iD[m]), c = F64(iD[d]);
			double res;
			switch ((q << 2) | (r << 1) | s)
			{
			case 0: res = c + a * b; break;		// VMLA
			case 1: res = c - a * b; break;		// VMLS
			case 2: res = -c + a * b; break;	// VNMLS
			case 3: res = -c - a * b; break;	// VNMLA
			case 4: res = a * b; break;			// VMUL
			case 5: res = -(a * b); break;		// VNMUL
			case 6: res = a + b; break;			// VADD
			default: res = a - b; break;		// VSUB
			}
			iD[d] = U64(res);
		}
		else
		{
			const float a = F32(GetS(n)), b = F32(GetS(m)), c = F32(GetS(d));
			float res;
			switch ((q << 2) | (r << 1) | s)
			{
			case 0: res = c + a * b; break;
			case 1: res = c - a * b; break;
			case 2: res = -c + a * b; break;
			case 3: res = -c - a * b; break;
			case 4: res = a * b; break;
			case 5: res = -(a * b); break;
			case 6: res = a + b; break;
			default: res = a - b; break;
			}
			SetS(d, U32(res));
		}
		return;
	}
	// extension space (p q r = 1 1 1)
	if (!s)										// VMOV immediate
	{
		const uint32_t imm8 = ((ins >> 12) & 0xF0) | (ins & 0xF);
		if (dbl) iD[d] = VfpImm64(imm8); else SetS(d, VfpImm32(imm8));
		return;
	}
	const uint32_t opc2 = (ins >> 16) & 0xF, b7 = (ins >> 7) & 1;
	switch (opc2)
	{
	case 0:										// VMOV reg (b7=0) / VABS (b7=1)
		if (dbl) iD[d] = b7 ? (iD[m] & 0x7FFFFFFFFFFFFFFFull) : iD[m];
		else SetS(d, b7 ? (GetS(m) & 0x7FFFFFFFu) : GetS(m));
		return;
	case 1:										// VNEG (b7=0) / VSQRT (b7=1)
		if (!b7)
		{
			if (dbl) iD[d] = iD[m] ^ 0x8000000000000000ull; else SetS(d, GetS(m) ^ 0x80000000u);
		}
		else if (dbl) iD[d] = U64(__builtin_sqrt(F64(iD[m])));
		else SetS(d, U32(__builtin_sqrtf(F32(GetS(m)))));
		return;
	case 4:										// VCMP Dd, Dm
	case 5:										// VCMP Dd, #0
		if (dbl) VfpCompare<double>(iFpscr, F64(iD[d]), opc2 == 5 ? 0.0 : F64(iD[m]));
		else VfpCompare<float>(iFpscr, F32(GetS(d)), opc2 == 5 ? 0.0f : F32(GetS(m)));
		return;
	case 7:										// VCVT between double and single
		if (dbl) SetS(((vd << 1) | D), U32((float)F64(iD[(M << 4) | vm])));
		else iD[(D << 4) | vd] = U64((double)F32(GetS((vm << 1) | M)));
		return;
	case 8:										// VCVT int -> float (b7: 1 = signed)
	{
		const uint32_t src = GetS((vm << 1) | M);
		if (dbl) iD[(D << 4) | vd] = U64(b7 ? (double)(int32_t)src : (double)src);
		else SetS(d, U32(b7 ? (float)(int32_t)src : (float)src));
		return;
	}
	case 12:									// VCVT float -> unsigned int
	case 13:									// VCVT float -> signed int
	{
		const double v = dbl ? F64(iD[(M << 4) | vm]) : (double)F32(GetS((vm << 1) | M));
		uint32_t res;
		if (v != v) res = 0;
		else if (opc2 == 13)
		{
			const double t = __builtin_trunc(v);
			res = t >= 2147483647.0 ? 0x7FFFFFFFu : t <= -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)t;
		}
		else
		{
			const double t = __builtin_trunc(v);
			res = t >= 4294967295.0 ? 0xFFFFFFFFu : t <= 0 ? 0u : (uint32_t)t;
		}
		SetS((vd << 1) | D, res);
		return;
	}
	default:
		VfpUndefined(pc, ins);
	}
}

void CArmCpu::Run(uint64_t aMaxSteps)
{
	const uint64_t start = iSteps;
	while (iHalt == ERunning)
	{
		Step();
		if (iOnProgress && !(iSteps & ((1u << 26) - 1))) iOnProgress(*this, iBpCtx);
		if (iSteps - start >= aMaxSteps)
		{
			Halt(EStepLimit);
			break;
		}
	}
}
}

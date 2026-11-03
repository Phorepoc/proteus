// thumb.cpp -- OURS (Proteus P2, user decision 2026-10-17: a Thumb-1 decoder now, dynarmic later). The ARMv5T Thumb instruction set
// (ARM ARM v5, chapter A6: the 19 16-bit formats, BL/BLX as two halfwords, BX/BLX interworking). In Thumb state R[15] holds the address
// of the NEXT halfword while one executes, so reading PC = R[15] + 2 (= this instruction + 4). Not ARMv6 Thumb (SXTH/REV/CPS...) or Thumb-2.
#include "armcpu.h"
#include <string.h>

namespace proteus
{
static inline uint32_t Bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

// (C++98: the x32 toolchain build has no lambdas)
static void NZ(CArmCpu& c, uint32_t v) { c.iN = v >> 31; c.iZ = v == 0; }
static uint32_t AddC(CArmCpu& c, uint32_t a, uint32_t b, uint32_t cin)
{
	const uint64_t res = (uint64_t)a + b + cin;
	const uint32_t v = (uint32_t)res;
	c.iN = v >> 31; c.iZ = v == 0; c.iC = (uint32_t)(res >> 32) & 1; c.iV = (~(a ^ b) & (a ^ v)) >> 31;
	return v;
}
// register-specified shift (ALU format: amount = Rs[7:0]), sets C like ARM
static uint32_t Shift(CArmCpu& c, uint32_t type, uint32_t v, uint32_t amt)
{
	if (amt == 0) return v;
	switch (type)
	{
	case 0: if (amt < 32) { c.iC = (v >> (32 - amt)) & 1; return v << amt; } c.iC = amt == 32 ? v & 1 : 0; return 0;
	case 1: if (amt < 32) { c.iC = (v >> (amt - 1)) & 1; return v >> amt; } c.iC = amt == 32 ? v >> 31 : 0; return 0;
	case 2: if (amt < 32) { c.iC = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); } c.iC = v >> 31; return c.iC ? 0xFFFFFFFFu : 0;
	default: { const uint32_t a = amt & 31; if (!a) { c.iC = v >> 31; return v; } c.iC = (v >> (a - 1)) & 1; return (v >> a) | (v << (32 - a)); }
	}
}

void CArmCpu::ThumbStep(uint32_t pc)
{
	const uint32_t ins = Read16(pc);
	if (iHalt != ERunning) return;
	R[15] = pc + 2;
	++iSteps;
	const uint32_t pcRead = pc + 4;
	uint32_t* r = R;
	switch (ins >> 13)
	{
	case 0:
		if (Bits(ins, 12, 11) != 3)		// format 1: LSL/LSR/ASR Rd, Rm, #imm5
		{
			const uint32_t type = Bits(ins, 12, 11), imm = Bits(ins, 10, 6), rm = r[Bits(ins, 5, 3)];
			uint32_t v;
			if (type == 0) v = imm ? Shift(*this, 0, rm, imm) : rm;
			else v = Shift(*this, type, rm, imm ? imm : 32);
			r[Bits(ins, 2, 0)] = v;
			NZ(*this, v);
		}
		else							// format 2: ADD/SUB Rd, Rn, Rm|#imm3
		{
			const uint32_t a = r[Bits(ins, 5, 3)], b = (ins & (1u << 10)) ? Bits(ins, 8, 6) : r[Bits(ins, 8, 6)];
			r[Bits(ins, 2, 0)] = (ins & (1u << 9)) ? AddC(*this, a, ~b, 1) : AddC(*this, a, b, 0);
		}
		return;
	case 1:								// format 3: MOV/CMP/ADD/SUB Rd, #imm8
	{
		const uint32_t rd = Bits(ins, 10, 8), imm = ins & 0xFF;
		switch (Bits(ins, 12, 11))
		{
		case 0: r[rd] = imm; NZ(*this, imm); break;
		case 1: AddC(*this, r[rd], ~imm, 1); break;
		case 2: r[rd] = AddC(*this, r[rd], imm, 0); break;
		default: r[rd] = AddC(*this, r[rd], ~imm, 1); break;
		}
		return;
	}
	case 2:
		if ((ins >> 10) == 0x10)		// format 4: ALU Rd, Rs
		{
			const uint32_t rd = Bits(ins, 2, 0), rs = Bits(ins, 5, 3), a = r[rd], b = r[rs];
			uint32_t v;
			switch (Bits(ins, 9, 6))
			{
			case 0: v = a & b; NZ(*this, v); r[rd] = v; break;					// AND
			case 1: v = a ^ b; NZ(*this, v); r[rd] = v; break;					// EOR
			case 2: v = Shift(*this, 0, a, b & 0xFF); NZ(*this, v); r[rd] = v; break;		// LSL
			case 3: v = Shift(*this, 1, a, b & 0xFF); NZ(*this, v); r[rd] = v; break;		// LSR
			case 4: v = Shift(*this, 2, a, b & 0xFF); NZ(*this, v); r[rd] = v; break;		// ASR
			case 5: r[rd] = AddC(*this, a, b, iC); break;						// ADC
			case 6: r[rd] = AddC(*this, a, ~b, iC); break;						// SBC
			case 7: v = Shift(*this, 3, a, b & 0xFF); NZ(*this, v); r[rd] = v; break;		// ROR
			case 8: NZ(*this, a & b); break;									// TST
			case 9: r[rd] = AddC(*this, 0, ~b, 1); break;						// NEG
			case 10: AddC(*this, a, ~b, 1); break;								// CMP
			case 11: AddC(*this, a, b, 0); break;								// CMN
			case 12: v = a | b; NZ(*this, v); r[rd] = v; break;					// ORR
			case 13: v = a * b; NZ(*this, v); r[rd] = v; break;					// MUL
			case 14: v = a & ~b; NZ(*this, v); r[rd] = v; break;					// BIC
			default: v = ~b; NZ(*this, v); r[rd] = v; break;						// MVN
			}
			return;
		}
		if ((ins >> 10) == 0x11)		// format 5: hi-register ADD/CMP/MOV, BX/BLX
		{
			const uint32_t rd = Bits(ins, 2, 0) | (Bits(ins, 7, 7) << 3), rm = Bits(ins, 6, 3);
			const uint32_t b = rm == 15 ? pcRead : r[rm];
			switch (Bits(ins, 9, 8))
			{
			case 0: { const uint32_t v = (rd == 15 ? pcRead : r[rd]) + b; if (rd == 15) Branch((v & ~1u) | 1); else r[rd] = v; break; }	// ADD (stays Thumb)
			case 1: AddC(*this, rd == 15 ? pcRead : r[rd], ~b, 1); break;							// CMP
			case 2: if (rd == 15) Branch((b & ~1u) | 1); else r[rd] = b; break;				// MOV (MOV pc stays Thumb)
			default:																	// BX / BLX Rm
				if (ins & 0x80) r[14] = (pc + 2) | 1;
				Branch(b);
				break;
			}
			return;
		}
		if ((ins >> 11) == 9)			// format 6: LDR Rd, [PC, #imm8*4]
		{
			r[Bits(ins, 10, 8)] = Read32((pcRead & ~3u) + (ins & 0xFF) * 4);
			return;
		}
		{								// formats 7/8: load/store with register offset
			const uint32_t addr = r[Bits(ins, 5, 3)] + r[Bits(ins, 8, 6)], rd = Bits(ins, 2, 0);
			switch (Bits(ins, 11, 9))
			{
			case 0: Write32(addr, r[rd]); break;							// STR
			case 1: Write16(addr, (uint16_t)r[rd]); break;					// STRH
			case 2: Write8(addr, (uint8_t)r[rd]); break;					// STRB
			case 3: r[rd] = (uint32_t)(int32_t)(int8_t)Read8(addr); break;	// LDRSB
			case 4: r[rd] = Read32(addr); break;							// LDR
			case 5: r[rd] = Read16(addr); break;							// LDRH
			case 6: r[rd] = Read8(addr); break;								// LDRB
			default: r[rd] = (uint32_t)(int32_t)(int16_t)Read16(addr); break;	// LDRSH
			}
			return;
		}
	case 3:								// format 9: LDR/STR (word: imm5*4, byte: imm5)
	{
		const bool byte = ins & (1u << 12), load = ins & (1u << 11);
		const uint32_t rd = Bits(ins, 2, 0), addr = r[Bits(ins, 5, 3)] + Bits(ins, 10, 6) * (byte ? 1 : 4);
		if (load) r[rd] = byte ? Read8(addr) : Read32(addr);
		else if (byte) Write8(addr, (uint8_t)r[rd]);
		else Write32(addr, r[rd]);
		return;
	}
	case 4:
		if (!(ins & (1u << 12)))		// format 10: LDRH/STRH imm5*2
		{
			const uint32_t rd = Bits(ins, 2, 0), addr = r[Bits(ins, 5, 3)] + Bits(ins, 10, 6) * 2;
			if (ins & (1u << 11)) r[rd] = Read16(addr); else Write16(addr, (uint16_t)r[rd]);
		}
		else							// format 11: SP-relative LDR/STR
		{
			const uint32_t rd = Bits(ins, 10, 8), addr = r[13] + (ins & 0xFF) * 4;
			if (ins & (1u << 11)) r[rd] = Read32(addr); else Write32(addr, r[rd]);
		}
		return;
	case 5:
		if (!(ins & (1u << 12)))		// format 12: ADD Rd, PC|SP, #imm8*4
		{
			r[Bits(ins, 10, 8)] = ((ins & (1u << 11)) ? r[13] : (pcRead & ~3u)) + (ins & 0xFF) * 4;
			return;
		}
		if ((ins & 0xFF00) == 0xB000)	// format 13: ADD/SUB SP, #imm7*4
		{
			const uint32_t off = (ins & 0x7F) * 4;
			r[13] = (ins & 0x80) ? r[13] - off : r[13] + off;
			return;
		}
		if ((ins & 0xF600) == 0xB400)	// format 14: PUSH {rlist[, LR]} / POP {rlist[, PC]}
		{
			const uint32_t list = ins & 0xFF;
			const bool extra = ins & 0x100;
			int n = extra;
			for (int i = 0; i < 8; ++i) n += (list >> i) & 1;
			if (!(ins & 0x800))			// PUSH: full descending
			{
				uint32_t a = r[13] - 4 * n;
				r[13] = a;
				for (int i = 0; i < 8; ++i) if (list & (1u << i)) { Write32(a, r[i]); a += 4; }
				if (extra) Write32(a, r[14]);
			}
			else
			{
				uint32_t a = r[13];
				for (int i = 0; i < 8; ++i) if (list & (1u << i)) { r[i] = Read32(a); a += 4; }
				uint32_t newPc = 0;
				if (extra) { newPc = Read32(a); a += 4; }
				r[13] = a;
				if (extra) Branch(newPc);		// ARMv5: POP {pc} interworks
			}
			return;
		}
		if ((ins & 0xFF00) == 0xBE00)	// BKPT
		{
			Halt(EUndefined); iFaultAddr = pc; strncpy(iNote, "Thumb BKPT", sizeof(iNote) - 1);
			return;
		}
		break;
	case 6:
		if (!(ins & (1u << 12)))		// format 15: STMIA/LDMIA Rb!, {rlist}
		{
			const uint32_t rb = Bits(ins, 10, 8), list = ins & 0xFF;
			uint32_t a = r[rb];
			if (ins & (1u << 11))
			{
				for (int i = 0; i < 8; ++i) if (list & (1u << i)) { r[i] = Read32(a); a += 4; }
				if (!(list & (1u << rb))) r[rb] = a;
			}
			else
			{
				for (int i = 0; i < 8; ++i) if (list & (1u << i)) { Write32(a, r[i]); a += 4; }
				r[rb] = a;
			}
			return;
		}
		{								// format 16/17: B<cond> / SWI
			const uint32_t cond = Bits(ins, 11, 8);
			if (cond == 0xF)
			{
				if (!iSvc || !iSvc(*this, ins & 0xFF, iCtx))
				{
					Halt(ESvcUnhandled); iFaultAddr = pc;
				}
				return;
			}
			if (cond == 0xE) break;		// undefined
			if (Cond(cond)) Branch((pcRead + ((int32_t)(int8_t)(ins & 0xFF) << 1)) | 1);
			return;
		}
	default:							// case 7: format 18 B, format 19 BL/BLX halves
		switch (Bits(ins, 12, 11))
		{
		case 0: Branch((pcRead + ((int32_t)(ins << 21) >> 20)) | 1); return;		// B: signed 11-bit halfword offset
		case 2: r[14] = pcRead + ((int32_t)(ins << 21) >> 9); return;				// BL/BLX prefix: LR = PC + (offset_hi << 12)
		case 3: { const uint32_t t = r[14] + ((ins & 0x7FF) << 1); r[14] = (pc + 2) | 1; Branch(t | 1); return; }	// BL suffix
		default:																	// BLX suffix: to ARM, word aligned
		{
			if (ins & 1) break;
			const uint32_t t = (r[14] + ((ins & 0x7FF) << 1)) & ~3u;
			r[14] = (pc + 2) | 1;
			Branch(t);
			return;
		}
		}
		break;
	}
	Halt(EUndefined);
	iFaultAddr = pc;
	strncpy(iNote, "undefined Thumb instruction", sizeof(iNote) - 1);
}
}

// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armcpu.h -- OURS (Proteus P0, docs/phorepoc/research/proteus-x86.md): a small ARMv5TE user-mode interpreter. Pure C++ (no Symbian types), so it builds on the host for
// tests (tests/proteus) and as an x32 Phorepoc EXE. It is the P0 stand-in for the dynarmic translator of P1: the same boundary (guest memory callbacks, SVC and
// "call into the native system" hooks), ARM mode + Thumb-1 (ARMv5T; user decision 2026-10-17: a Thumb-1 decoder now, dynarmic later).
//
// Guest addresses are 32 bits. The guest sees ONE flat region [iBase, iBase + iSize) of host memory (iHost); on x32 Phorepoc guest address == host address
// (ILP32), on an LP64 host test it is an offset. Accesses outside the region stop the CPU with a fault.
#ifndef PROTEUS_ARMCPU_H
#define PROTEUS_ARMCPU_H

#include <stdint.h>

namespace proteus
{
enum { KTrapBase = 0xF0000000u, KTrapEnd = 0xF8000000u, KReturnToHost = 0xF8000000u };		// trap = KTrapBase | import slot << 2 (proteusrt.h)

enum THalt { ERunning = 0, EReturned, EExited, EFault, EUndefined, EThumb, EStepLimit, ESvcUnhandled, EHleStop };

struct FastCore;
struct TArmJit;

class CArmCpu
{
	friend struct FastCore;
public:
	CArmCpu();
	uint32_t R[16];
	uint32_t iN, iZ, iC, iV;
	uint32_t iT;				// CPSR T: executing Thumb (R[15] holds the halfword address, bit 0 clear)
	uint32_t iGE;				// CPSR GE[3:0] (ARMv6 parallel add/subtract -> SEL)
	uint32_t iTpidrurw;			// CP15 c13 c0 2, the user read/write thread ID: EKA2 keeps the thread's TLocalThreadData address there (u32std.h)
	uint32_t iTpidruro;			// CP15 c13 c0 3, user read-only
	// VFP (rung 30): the double registers d0-d31 (s0-s31 are the halves of d0-d15), FPSCR/FPEXC; ARM-mode VFPv3 subset, executed with host float/double
	uint64_t iD[32];
	uint32_t iFpscr, iFpexc;
	// guest memory
	uint8_t* iHost;
	uint32_t iBase, iSize;
	// the host memory behind the region may be only reserved (the board: a disconnected chunk, committed as the guest commits, the way
	// the kernel commits a process's chunks). Null = all of it is usable (the host builds). Offsets are region-relative, page aligned.
	typedef bool (*TCommitFn)(uint32_t aOffset, uint32_t aSize, bool aCommit, void* aCtx);
	TCommitFn iCommit;
	void* iCommitCtx;
	uint8_t* iPageMap;			// with iCommit: one byte per region page, 1 = committed (a guest access to any other page faults)
	bool Commit(uint32_t aAddr, uint32_t aLen, bool aCommit = true)
	{
		if (!iCommit || !aLen) return true;
		if (!InRegion(aAddr, aLen)) return false;
		const uint32_t a0 = (aAddr - iBase) & ~0xFFFu, a1 = (aAddr - iBase + aLen + 0xFFF) & ~0xFFFu;
		if (!iCommit(a0, a1 - a0, aCommit, iCommitCtx)) return false;
		for (uint32_t p = a0 >> 12; p < a1 >> 12; ++p) iPageMap[p] = aCommit;
		PagesChanged(iBase + a0, a1 - a0);
		return true;
	}
	bool Committed(uint32_t aAddr, uint32_t aLen) const
	{
		if (!iPageMap || !aLen) return true;
		for (uint32_t p = (aAddr - iBase) >> 12, e = (aAddr - iBase + aLen - 1) >> 12; p <= e; ++p) if (!iPageMap[p]) return false;
		return true;
	}
	// hooks (set by the embedder)
	typedef void (*TTrapFn)(CArmCpu& aCpu, uint32_t aAddr, void* aCtx);			// pc entered [KTrapBase, KTrapEnd): emulate the function, then return (BX lr) with SetReturn()
	typedef bool (*TSvcFn)(CArmCpu& aCpu, uint32_t aImm, void* aCtx);			// false = not handled
	TTrapFn iTrap;
	// debug breakpoints: pc == iBp[i] (before executing) calls iOnBp (logging only; execution continues)
	enum { KMaxBp = 8 };
	uint32_t iBp[KMaxBp];
	int iBpCount;
	void (*iOnBp)(CArmCpu& aCpu, int aIndex, void* aCtx);
	void* iBpCtx;
	// every 2^26 instructions (a long run shows where it is: a busy guest vs one spinning in a loop); iBpCtx is its context too
	void (*iOnProgress)(CArmCpu& aCpu, void* aCtx);
	// the time slice: every iSliceSteps instructions the embedder is called between two instructions (the registers are a consistent
	// guest state there) and may switch the guest to another thread by loading its registers; 0 = off. iBpCtx is its context too.
	void (*iOnSlice)(CArmCpu& aCpu, void* aCtx);
	uint64_t iSliceSteps, iNextSlice;
	// the local exclusive monitor (LDREX sets it, STREX needs it, a thread switch clears it)
	bool iExcl;
	// pc outside the guest region (a call through a native object's vtable, e.g. a CBitmapFont in the font server's shared heap): the
	// embedder returns the guest code to run instead (BX semantics, bit 0 = Thumb) or 0 (the fetch faults as before); iBpCtx is its context
	uint32_t (*iOnForeignPc)(CArmCpu& aCpu, uint32_t aPc, void* aCtx);
	// debug store watchpoint: a guest store (incl. emulated execs writing guest memory) into [iWatchLo, iWatchHi) calls iOnWatch
	uint32_t iWatchLo, iWatchHi;
	void (*iOnWatch)(CArmCpu& aCpu, uint32_t aAddr, uint32_t aValue, int aSize, void* aCtx);
	TSvcFn iSvc;
	void* iCtx;
	// P1: the translator (armjit.h, builds with PROTEUS_JIT) runs the guest instead of the interpreter while it is set; breakpoints and
	// watchpoints keep the interpreter. Null = the interpreter (the default, and the fallback when the translator cannot start).
	TArmJit* iJit;
	void PagesChanged(uint32_t aAddr, uint32_t aLen);							// the region's committed pages or the windows changed
	void CodeChanged(uint32_t aAddr, uint32_t aLen);							// guest code was written or removed there (a module, an IMB)
	// state
	THalt iHalt;
	int32_t iExitCode;
	uint64_t iSteps;
	uint64_t iStatSlow, iStatSvc, iStatTrap, iStatMem;		// statistics (progress line): instructions that left the fast loop for Step(), SWIs, trap-range entries, guest loads/stores that left the region fast path
	uint32_t iFaultAddr;
	uint32_t iLastPc;			// the instruction being executed (a Thumb branch halts with pc = the target; this is the branch)
	char iNote[96];
	uint32_t iGuestAlloc = 0;	// (R1, native euser) the allocator the guest switched this thread to; 0 = the handler's own (bridge.cpp TGuestHeap)

	// guest memory access (false/0 + fault on a bad address)
	bool InRegion(uint32_t aAddr, uint32_t aLen) const { return aAddr >= iBase && aLen <= iSize && aAddr - iBase <= iSize - aLen; }
	bool Valid(uint32_t aAddr, uint32_t aLen) const { return (InRegion(aAddr, aLen) && Committed(aAddr, aLen)) || Window(aAddr, aLen) >= 0; }
	// P2: native mappings the guest may touch besides its region (a global chunk opened for it, ...): guest address == host address
	// there (x32/ILP32), so these exist only on the board. A stray pointer outside region + windows still faults.
	enum { KMaxWindows = 32 };
	uint32_t iWinBase[KMaxWindows], iWinSize[KMaxWindows];
	int iWinCount;
	int Window(uint32_t aAddr, uint32_t aLen) const
	{
		for (int i = 0; i < iWinCount; ++i)
			if (aAddr >= iWinBase[i] && aLen <= iWinSize[i] && aAddr - iWinBase[i] <= iWinSize[i] - aLen) return i;
		return -1;
	}
	bool AddWindow(uint32_t aBase, uint32_t aSize);
	uint8_t* HostPtr(uint32_t aAddr) const;		// region: iHost-relative; window: the address itself
	uint32_t Read32(uint32_t aAddr);
	uint16_t Read16(uint32_t aAddr);
	uint8_t Read8(uint32_t aAddr);
	void Write32(uint32_t aAddr, uint32_t aVal);
	void Write16(uint32_t aAddr, uint16_t aVal);
	void Write8(uint32_t aAddr, uint8_t aVal);
	const char* CString(uint32_t aAddr, char* aBuf, int aMax);					// copy a guest C string (bounded)

	void SetReturn() { Branch(R[14]); }												// emulated function done: continue at lr (bit 0 = back to Thumb)
	void Branch(uint32_t aTarget);													// BX semantics: bit 0 selects Thumb
	void Halt(THalt aWhy, int32_t aCode = 0) { if (iHalt == ERunning) { iHalt = aWhy; iExitCode = aCode; } }
	void Run(uint64_t aMaxSteps);
	uint32_t GetS(int n) const;
	void SetS(int n, uint32_t v);
private:
	void VfpTransfer(uint32_t ins, uint32_t pc);
	void VfpOp(uint32_t ins, uint32_t pc);
	void VfpOpScalar(uint32_t ins, uint32_t pc);
	void VfpUndefined(uint32_t pc, uint32_t ins);
	void Step();
	bool Cond(uint32_t aCond) const;
	uint32_t Operand2(uint32_t aIns, bool& aCarry);
	void DataProc(uint32_t aIns);
	void Multiply(uint32_t aIns);
	void HalfwordTransfer(uint32_t aIns);
	void SingleTransfer(uint32_t aIns);
	void BlockTransfer(uint32_t aIns);
	void Media(uint32_t aIns, uint32_t aPc);											// ARMv6/v7 media group (cond 011x ... bit 4 = 1)
	void ParallelAddSub(uint32_t aIns, uint32_t aPc);
	void ThumbStep(uint32_t aPc);
	uint32_t Reg(uint32_t aN, uint32_t aPcOffset) const { return aN == 15 ? R[15] + aPcOffset : R[aN]; }
	void Fault(uint32_t aAddr, const char* aWhat);
};
}

#endif

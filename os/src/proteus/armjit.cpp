// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armjit.cpp -- OURS (Proteus P1): see armjit.h. The seams to dynarmic:
//   memory     a page table over the guest region's committed pages and the windows (guest address -> host page); everything else (an
//              uncommitted page, a stray pointer) takes the callbacks, i.e. CArmCpu::Read32/Write32 and their faults
//   fetch      MemoryReadCode answers only for committed region pages; a fetch anywhere else (the HLE trap range, KReturnToHost, a native
//              object's vtable = a foreign pc) raises NoExecuteFault at that pc, handled exactly like CArmCpu::Step does
//   SVC        CallSVC -> CArmCpu::iSvc with the state copied out; the JIT stops after every SVC, trap and fault, and the caller's loop
//              reloads the state (an exec call may have switched the guest thread, halted, or moved the pc)
//   CP15       c13 c0 2/3 (TPIDRURW/TPIDRURO) read and written in place in CArmCpu, the only user-visible CP15 registers (as the interpreter)
//   time       AddTicks/GetTicksRemaining count guest instructions into CArmCpu::iSteps, so slices and step limits keep their meaning
#ifdef PROTEUS_JIT
#include "armjit.h"
#include "armcpu.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <memory>
#include <string.h>
#include <stdio.h>
#include <time.h>

namespace proteus
{
using namespace Dynarmic;

namespace
{
class TCp15 final : public A32::Coprocessor
{
public:
	explicit TCp15(CArmCpu& aCpu) : iCpu(aCpu) {}
	std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg, unsigned) override { return std::nullopt; }
	CallbackOrAccessOneWord CompileSendOneWord(bool aTwo, unsigned aOpc1, A32::CoprocReg aCRn, A32::CoprocReg aCRm, unsigned aOpc2) override
	{
		if (!aTwo && aOpc1 == 0 && aCRn == A32::CoprocReg::C13 && aCRm == A32::CoprocReg::C0 && aOpc2 == 2) return &iCpu.iTpidrurw;
		return std::monostate{};
	}
	CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
	CallbackOrAccessOneWord CompileGetOneWord(bool aTwo, unsigned aOpc1, A32::CoprocReg aCRn, A32::CoprocReg aCRm, unsigned aOpc2) override
	{
		if (!aTwo && aOpc1 == 0 && aCRn == A32::CoprocReg::C13 && aCRm == A32::CoprocReg::C0)
		{
			if (aOpc2 == 2) return &iCpu.iTpidrurw;
			if (aOpc2 == 3) return &iCpu.iTpidruro;
		}
		return std::monostate{};
	}
	CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
	std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
	std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
private:
	CArmCpu& iCpu;
};
}

struct TArmJit final : public A32::UserCallbacks
{
	CArmCpu& iCpu;
	std::unique_ptr<std::array<std::uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> iPages;
	ExclusiveMonitor iMonitor{1};	// LDREX/STREX: one processor (the guest threads share this CPU object)
	std::unique_ptr<A32::Jit> iJit;
	uint64_t iLimit = 0;
	uint32_t iQ = 0;				// CPSR.Q: the interpreter keeps no sticky saturation flag, the JIT keeps it across reloads
	bool iCpuOwns = false;			// a hook copied the state out and may have changed it: CArmCpu holds the truth, reload before running on
	uint32_t iWinReports = 0;		// 10 s reports so far (the first 60: bounded serial output)
	time_t iHookSecs = 0;			// wall time this run spent inside SVC hooks (slow-run diagnostics)
	uint32_t iRunSvc = 0, iRunSvcImm = 0;	// SVCs this run, the last one's number
	uint64_t iRunTicks = 0;			// AddTicks this run
	time_t iWinStart = 0, iWinHook = 0;	// the 10 s report: window start, seconds in SVC hooks
	uint32_t iWinRuns = 0, iWinSvc = 0;
	uint64_t iWinTicks = 0;				// Jit::Run calls: every 64 K of them the progress hook reports (a guest that leaves the JIT constantly runs few instructions)

	explicit TArmJit(CArmCpu& aCpu) : iCpu(aCpu) {}

	// --- state between CArmCpu and the JIT ---
	void Load()
	{
		auto& r = iJit->Regs();
		for (int i = 0; i < 16; ++i) r[i] = iCpu.R[i];
		iJit->SetCpsr((iCpu.iN << 31) | (iCpu.iZ << 30) | (iCpu.iC << 29) | (iCpu.iV << 28) | iQ | ((iCpu.iGE & 0xF) << 16) | (iCpu.iT << 5) | 0x10);
		auto& x = iJit->ExtRegs();
		for (int i = 0; i < 32; ++i) { x[2 * i] = (uint32_t)iCpu.iD[i]; x[2 * i + 1] = (uint32_t)(iCpu.iD[i] >> 32); }
		iJit->SetFpscr(iCpu.iFpscr);
		iJit->ClearExclusiveState();
		iMonitor.ClearProcessor(0);		// a reload may be another guest thread (the interpreter clears iExcl on a switch)
		iCpuOwns = false;
	}
	void Store()
	{
		const auto& r = iJit->Regs();
		for (int i = 0; i < 16; ++i) iCpu.R[i] = r[i];
		iCpu.iLastPc = r[15];		// out of ticks: where the guest is (progress lines, thread stats); the hooks set the exact instruction after this
		const uint32_t c = iJit->Cpsr();
		iCpu.iN = c >> 31; iCpu.iZ = (c >> 30) & 1; iCpu.iC = (c >> 29) & 1; iCpu.iV = (c >> 28) & 1; iQ = c & (1u << 27);
		iCpu.iGE = (c >> 16) & 0xF; iCpu.iT = (c >> 5) & 1;
		const auto& x = iJit->ExtRegs();
		for (int i = 0; i < 32; ++i) iCpu.iD[i] = x[2 * i] | ((uint64_t)x[2 * i + 1] << 32);
		iCpu.iFpscr = iJit->Fpscr();
	}
	void HandOver()					// a hook needs the state: copy it out, stop the JIT after this callback
	{
		Store();
		iCpuOwns = true;
		iJit->HaltExecution();
	}

	// --- memory ---
	std::uint8_t MemoryRead8(A32::VAddr a) override { return iCpu.Read8(a); }
	std::uint16_t MemoryRead16(A32::VAddr a) override { return iCpu.Read16(a); }
	std::uint32_t MemoryRead32(A32::VAddr a) override { return iCpu.Read32(a); }
	std::uint64_t MemoryRead64(A32::VAddr a) override { return iCpu.Read32(a) | ((uint64_t)iCpu.Read32(a + 4) << 32); }
	void MemoryWrite8(A32::VAddr a, std::uint8_t v) override { iCpu.Write8(a, v); }
	void MemoryWrite16(A32::VAddr a, std::uint16_t v) override { iCpu.Write16(a, v); }
	void MemoryWrite32(A32::VAddr a, std::uint32_t v) override { iCpu.Write32(a, v); }
	void MemoryWrite64(A32::VAddr a, std::uint64_t v) override { iCpu.Write32(a, (uint32_t)v); iCpu.Write32(a + 4, (uint32_t)(v >> 32)); }
	bool MemoryWriteExclusive8(A32::VAddr a, std::uint8_t v, std::uint8_t) override { iCpu.Write8(a, v); return true; }
	bool MemoryWriteExclusive16(A32::VAddr a, std::uint16_t v, std::uint16_t) override { iCpu.Write16(a, v); return true; }
	bool MemoryWriteExclusive32(A32::VAddr a, std::uint32_t v, std::uint32_t) override { iCpu.Write32(a, v); return true; }
	bool MemoryWriteExclusive64(A32::VAddr a, std::uint64_t v, std::uint64_t) override { MemoryWrite64(a, v); return true; }
	std::optional<std::uint32_t> MemoryReadCode(A32::VAddr a) override
	{
		if (!iCpu.InRegion(a, 4) || !iCpu.Committed(a, 4)) return std::nullopt;		// trap, return-to-host, foreign pc, a stray jump: NoExecuteFault
		uint32_t w;
		memcpy(&w, iCpu.iHost + (a - iCpu.iBase), 4);
		return w;
	}

	// --- hooks ---
	void InterpreterFallback(A32::VAddr aPc, size_t) override
	{
		HandOver();
		iCpu.Halt(EUndefined);
		iCpu.iFaultAddr = aPc;
		iCpu.iLastPc = aPc;
		strncpy(iCpu.iNote, "JIT: no translation for this instruction", sizeof(iCpu.iNote) - 1);
	}
	void CallSVC(std::uint32_t aImm) override
	{
		HandOver();				// R[15] = the next instruction, as the interpreter's SWI sees it
		++iCpu.iStatSvc;
		++iRunSvc; iRunSvcImm = aImm;
		const time_t t0 = time(nullptr);
		if (!iCpu.iSvc || !iCpu.iSvc(iCpu, aImm, iCpu.iCtx))
		{
			iCpu.Halt(ESvcUnhandled, (int32_t)aImm);
			iCpu.iFaultAddr = iCpu.R[15] - (iCpu.iT ? 2 : 4);
		}
		iHookSecs += time(nullptr) - t0;
	}
	void ExceptionRaised(A32::VAddr aPc, A32::Exception aWhy) override
	{
		HandOver();
		iCpu.R[15] = aPc;
		iCpu.iLastPc = aPc;
		switch (aWhy)
		{
		case A32::Exception::NoExecuteFault:
			if (aPc >= KTrapBase && aPc < KTrapEnd)
			{
				++iCpu.iStatTrap;
				if (iCpu.iTrap) iCpu.iTrap(iCpu, aPc, iCpu.iCtx);
				else iCpu.Halt(EHleStop);
				return;
			}
			if (aPc == KReturnToHost)
			{
				iCpu.Halt(EReturned, (int32_t)iCpu.R[0]);
				return;
			}
			if (iCpu.iOnForeignPc && aPc - iCpu.iBase > iCpu.iSize - 2)
			{
				const uint32_t t = iCpu.iOnForeignPc(iCpu, aPc, iCpu.iBpCtx);
				if (t) { iCpu.Branch(t); return; }
			}
			iCpu.Halt(EFault);
			iCpu.iFaultAddr = aPc;
			strncpy(iCpu.iNote, "instruction fetch", sizeof(iCpu.iNote) - 1);
			return;
		case A32::Exception::Breakpoint:
		case A32::Exception::UndefinedInstruction:
		case A32::Exception::UnpredictableInstruction:
		case A32::Exception::DecodeError:
		default:
			iCpu.Halt(EUndefined);
			iCpu.iFaultAddr = aPc;
			strncpy(iCpu.iNote, aWhy == A32::Exception::Breakpoint ? "BKPT" : "undefined instruction (JIT)", sizeof(iCpu.iNote) - 1);
			return;
		}
	}
	void AddTicks(std::uint64_t aTicks) override { iCpu.iSteps += aTicks; iRunTicks += aTicks; }
	// dynarmic keeps the remaining ticks as a signed 64-bit count: hand it at most 2^30 per run (an unlimited budget is ~0)
	std::uint64_t GetTicksRemaining() override { const uint64_t r = iCpu.iSteps < iLimit ? iLimit - iCpu.iSteps : 0; return r < (1u << 30) ? r : (1u << 30); }

	// --- page table ---
	void MapPage(uint32_t aPage)
	{
		const uint32_t a = aPage << 12;
		std::uint8_t* host = nullptr;
		if (iCpu.InRegion(a, 0x1000) && iCpu.Committed(a, 0x1000)) host = iCpu.iHost + (a - iCpu.iBase);
		else if (iCpu.Window(a, 0x1000) >= 0) host = (std::uint8_t*)(uintptr_t)a;
		(*iPages)[aPage] = host;
	}
};

TArmJit* JitNew(CArmCpu& aCpu)
{
	if ((aCpu.iBase & 0xFFF) || ((uintptr_t)aCpu.iHost & 0xFFF)) return nullptr;		// the page table maps whole pages
	auto j = std::make_unique<TArmJit>(aCpu);
	j->iPages = std::make_unique<std::array<std::uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
	j->iPages->fill(nullptr);
	A32::UserConfig c;
	c.callbacks = j.get();
	c.page_table = j->iPages.get();
	c.absolute_offset_page_table = false;
	c.detect_misaligned_access_via_page_table = 16 | 32 | 64;		// an access across a page boundary takes the callbacks (the next page may be uncommitted)
	c.only_detect_misalignment_via_page_table_on_page_boundary = true;
	c.coprocessors[15] = std::make_shared<TCp15>(aCpu);
	c.global_monitor = &j->iMonitor;
	c.processor_id = 0;
	c.arch_version = A32::ArchVersion::v7;
	c.define_unpredictable_behaviour = true;
	c.always_little_endian = true;
	c.code_cache_size = 32u * 1024 * 1024;
	j->iJit = std::make_unique<A32::Jit>(c);
	for (uint32_t p = aCpu.iBase >> 12, e = (uint32_t)(((uint64_t)aCpu.iBase + aCpu.iSize) >> 12); p < e; ++p) j->MapPage(p);
	for (int w = 0; w < aCpu.iWinCount; ++w) JitMap(j.get(), aCpu.iWinBase[w], aCpu.iWinSize[w]);
	return j.release();
}

void JitDelete(TArmJit* aJit) { delete aJit; }

void JitMap(TArmJit* aJit, uint32_t aAddr, uint32_t aLen)
{
	if (!aJit || !aLen) return;
	const uint32_t p0 = aAddr >> 12, p1 = (uint32_t)(((uint64_t)aAddr + aLen + 0xFFF) >> 12);
	for (uint32_t p = p0; p < p1; ++p) aJit->MapPage(p);
	aJit->iJit->InvalidateCacheRange(p0 << 12, (size_t)(p1 - p0) << 12);
}

void JitInvalidate(TArmJit* aJit, uint32_t aAddr, uint32_t aLen)
{
	if (aJit && aLen) aJit->iJit->InvalidateCacheRange(aAddr, aLen);
}

void JitRun(TArmJit* aJit, uint64_t aLimit)
{
	CArmCpu& c = aJit->iCpu;
	aJit->iLimit = aLimit;
	while (c.iHalt == ERunning && c.iSteps < aLimit)
	{
		aJit->Load();
		aJit->iJit->ClearHalt();
		const time_t t0 = time(nullptr);
		aJit->iHookSecs = 0; aJit->iRunSvc = 0; aJit->iRunTicks = 0;
		const uint64_t budget = aJit->GetTicksRemaining();
		aJit->iJit->Run();
		const time_t now = time(nullptr), secs = now - t0;
		++aJit->iWinRuns; aJit->iWinSvc += aJit->iRunSvc; aJit->iWinTicks += aJit->iRunTicks; aJit->iWinHook += aJit->iHookSecs;
		if (!aJit->iWinStart) aJit->iWinStart = now;
		if (now - aJit->iWinStart >= 10 && aJit->iWinReports < 60)		// every 10 s of wall time this CPU runs (the first 60): runs, SVCs, guest instructions, seconds in SVC hooks
		{
			char w[200];
			snprintf(w, sizeof(w), "PH: proteus jit 10s: %u s: %u runs, %u SVCs, %llu K instr, %u s in SVC hooks, pc %08x",
				(unsigned)(now - aJit->iWinStart), aJit->iWinRuns, aJit->iWinSvc, (unsigned long long)(aJit->iWinTicks >> 10), (unsigned)aJit->iWinHook,
				(unsigned)(aJit->iCpuOwns ? c.R[15] : aJit->iJit->Regs()[15]));
			JitDiag(w);
			++aJit->iWinReports;
			aJit->iWinStart = now; aJit->iWinRuns = aJit->iWinSvc = 0; aJit->iWinTicks = 0; aJit->iWinHook = 0;
		}
		if (secs >= 2)		// a run that took 2 s or more of wall time: where it went (SVC hooks or the JIT itself: translation, guest code)
		{
			char b[300];
			snprintf(b, sizeof(b), "PH: proteus jit slow run: %u s, %u s in %u SVC hooks (last 0x%x), %llu ticks of %llu, owns %d halt %d, steps %llu limit %llu, pc %08x",
				(unsigned)secs, (unsigned)aJit->iHookSecs, aJit->iRunSvc, aJit->iRunSvcImm, (unsigned long long)aJit->iRunTicks,
				(unsigned long long)budget, aJit->iCpuOwns, (int)c.iHalt, (unsigned long long)c.iSteps, (unsigned long long)aLimit, (unsigned)(aJit->iCpuOwns ? c.R[15] : aJit->iJit->Regs()[15]));
			JitDiag(b);
		}
		if (!aJit->iCpuOwns) { aJit->Store(); return; }		// out of ticks: the caller's loop does slices, progress, the step limit
		// a hook ran (SVC, trap, fault): its effects are in CArmCpu; go on unless it halted or the time is up
	}
}
}
#endif

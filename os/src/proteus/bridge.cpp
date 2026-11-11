// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// bridge.cpp -- OURS (Proteus R1, docs: proteus-native-bridge.md): the ARM -> native system-library bridge. An import of a DLL that bridge.cfg
// lists as native does not run the ARM DLL (or the HLE): the import slot's trap calls the NATIVE x32 export of the same NAME. The arguments
// are read the way the AAPCS (soft-float) passes them (BrArgs: r0-r3, then the stack; a 64-bit value takes an even register pair or an
// 8-aligned slot), the generated trampoline (gen/bridge_shapes.inc: one per call shape) makes the SysV x32 call, the result goes in r0/r1.
// Pointers are the same 32-bit number on both sides: guest address == host address on the board (BridgeFlatMap makes the whole x32 user
// space guest memory; on the host tests a fake native table plays the native DLL).
//
// bridge.cfg (one line each, '#' comments):
//   native euser.dll         the DLL's imports go to the native export of the same name
//   native efsrv.dll
//   map <lo> <hi>            guest memory = the host address range [lo, hi) as it is (default 0x00010000 0xC0000000, board only)
//   trace                    log every bridged call (name, r0-r3)
#include "proteusrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && !defined(__SYMBIAN32__)
#include <sys/mman.h>
#endif

namespace proteus
{
// the AAPCS argument cursor the generated trampolines read from
struct BrArgs
{
	CArmCpu& c;
	int n;
	explicit BrArgs(CArmCpu& aCpu) : c(aCpu), n(0) {}
	uint32_t Raw(int i) { return i < 4 ? c.R[i] : c.Read32(c.R[13] + 4 * (i - 4)); }
	uint32_t W() { return Raw(n++); }
	uint64_t L() { if (n & 1) ++n; const uint64_t v = Raw(n) | (uint64_t)Raw(n + 1) << 32; n += 2; return v; }
	double D() { const uint64_t b = L(); double d; memcpy(&d, &b, 8); return d; }
	float F() { const uint32_t b = W(); float f; memcpy(&f, &b, 4); return f; }
	void S(void* aDst, unsigned aSize, unsigned aAlign)
	{
		if (aAlign > 4 && (n & 1)) ++n;
		for (unsigned i = 0; i < aSize; i += 4)
		{
			const uint32_t w = Raw(n++);
			memcpy((char*)aDst + i, &w, aSize - i < 4 ? aSize - i : 4);
		}
	}
};

// ---- calling guest code from native code (reverse stubs). A function pointer the guest hands to a native export (a comparator, a thread or
// exception handler, a TCallBack in a struct) is an ARM address the native side cannot call: BrCb gives it a native stub from a pool; the
// stub (called with up to 4 words, returning one -- every callback signature of euser and efsrv fits) runs the guest function on a nested
// interpreter run (BridgeCallGuest). A pointer outside the guest region is a native one already and passes through.
static void NestEnter(CArmCpu& aCpu);
static bool NestLeave(CArmCpu& aCpu, int32_t& aReason);
static void NativeLeave(int32_t aReason);
static void ForgetLeaveState(CArmCpu& aCpu);
static void ForgetGuestTls(CArmCpu& aCpu);

// ---- allocators (R1). With a native euser the guest's User::SwitchAllocator/SwitchHeap would switch the NATIVE thread's allocator, and
// everything Proteus and the translator allocate after it (operator new, User::Alloc: blocks, code-cache tables, CPUs) would land in the
// guest's heap -- a block made before the switch then freed into the guest's heap corrupts its free list (PyS60: Python's 1 MB ChunkHeap,
// round 14). So the thread runs on the handler's allocator while Proteus runs, and on the guest's one (CArmCpu::iGuestAlloc) only while
// native code runs for the guest: a bridged export or native virtual (TGuestHeap), and native code a reverse call returns to.
static TBridgeThreads gThr;		// the embedder's thread services (BridgeSetThreads)
struct TGuestHeap
{
	CArmCpu& iCpu;
	bool iOn;
	TGuestHeap(CArmCpu& aCpu, bool aWatch) : iCpu(aCpu), iOn(false)
	{
		if (!gThr.iSwitchAlloc) return;
		if (iCpu.iGuestAlloc) { gThr.iSwitchAlloc(iCpu.iGuestAlloc); iOn = true; }
		else iOn = aWatch;		// a SwitchAllocator/SwitchHeap row: read back what the guest switched to
	}
	~TGuestHeap()
	{
		if (!iOn) return;
		const uint32_t cur = gThr.iSwitchAlloc(gThr.iHostAlloc);
		iCpu.iGuestAlloc = cur == gThr.iHostAlloc ? 0 : cur;
	}
};
// native code -> guest code (a reverse call, a new guest thread): back on the handler's allocator; the guest's is the one native code ran on
static uint32_t HostHeapEnter()
{
	if (!gThr.iSwitchAlloc) return 0;
	const uint32_t cur = gThr.iSwitchAlloc(gThr.iHostAlloc);
	return cur == gThr.iHostAlloc ? 0 : cur;
}
static void HostHeapLeave(CArmCpu& aCpu, bool aEntered)
{	// on to the allocator the guest has now (it may have switched inside the call), for the native code the call returns to
	if (gThr.iSwitchAlloc && (aEntered || aCpu.iGuestAlloc)) gThr.iSwitchAlloc(aCpu.iGuestAlloc ? aCpu.iGuestAlloc : gThr.iHostAlloc);
}

uint32_t BridgeCallGuest(CArmCpu& aCpu, uint32_t aFn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	CArmCpu& c = aCpu;
	const uint32_t guestAlloc = HostHeapEnter();
	if (guestAlloc) c.iGuestAlloc = guestAlloc;
	uint32_t R[16], flags[6];
	uint64_t D[32];
	memcpy(R, c.R, sizeof(R)); memcpy(D, c.iD, sizeof(D));
	flags[0] = c.iN; flags[1] = c.iZ; flags[2] = c.iC; flags[3] = c.iV; flags[4] = c.iT; flags[5] = c.iGE;
	const uint32_t fpscr = c.iFpscr;
	const THalt halt = c.iHalt; const int32_t code = c.iExitCode;
	TArmJit* const jit = c.iJit; const uint64_t slice = c.iSliceSteps, next = c.iNextSlice;
	c.iJit = 0;									// the translator is not re-entrant: the nested call interprets
	c.iSliceSteps = 0;							// and is not preempted (no guest thread switch inside a native callback)
	const uint32_t sp = (R[13] - 64) & ~7u;
	c.R[0] = a0; c.R[1] = a1; c.R[2] = a2; c.R[3] = a3;
	c.R[13] = sp; c.R[14] = KReturnToHost;
	c.iHalt = ERunning;
	c.Branch(aFn);
	NestEnter(c);
	c.Run(~0ull);
	const uint32_t result = c.R[0];
	int32_t leave = 0;
	const bool left = NestLeave(c, leave);		// the guest left with no ARM TRAP inside this run: raise it natively, from here
	const bool ok = c.iHalt == EReturned || left;
	c.iJit = jit; c.iSliceSteps = slice; c.iNextSlice = next;
	if (ok)											// back to the interrupted guest code, exactly as it was
	{
		memcpy(c.R, R, sizeof(R)); memcpy(c.iD, D, sizeof(D));
		c.iN = flags[0]; c.iZ = flags[1]; c.iC = flags[2]; c.iV = flags[3]; c.iT = flags[4]; c.iGE = flags[5];
		c.iFpscr = fpscr; c.iHalt = halt; c.iExitCode = code;
	}
	HostHeapLeave(c, guestAlloc != 0);
	if (left)
		NativeLeave(leave);
	return ok ? result : 0;						// (anything else: the guest stopped in the callback; the halt stays and ends the run)
}

// ---- one guest CPU per native thread. A guest thread is a NATIVE thread: the guest's RThread::Create goes to the native euser with the thread
// function turned into a reverse stub (BrCb), the kernel starts a real thread, and its first entry into the stub finds no CPU for that thread:
// RunOnNewCpu gives it its own CArmCpu (a copy of the primary's memory view and hooks, its own registers, a guest stack from the native heap,
// its own translator when the embedder provides one) and runs the function to its return there. Guest memory is the one shared region/map;
// code is translated per CPU (dynarmic has no shared cache across instances). A thread that already has a CPU (the primary, or a guest thread
// calling native code that calls back) runs the callback nested on it. The cpu<->thread table is keyed by the embedder's thread id.
namespace
{
bool gTrace;
uint32_t gWatch, gWatchVal, gWatchPrev, gWatchSize; bool gWatchOn;	// `watchsize <hex>`: (re)arm on every allocation of that size
	// (armed when an allocation returns the cell right after the word)	// bridge.cfg `watch <hex addr>`: log every change of that word, seen at a bridged call (before = guest code wrote it)
bool gTraceHeap;
uint32_t gTraceDll;
char gTraceOnLoad[32];		// bridge.cfg `traceonload [name]`: the full trace from a guest RLibrary::Load (of a name containing it) on
		// bridge.cfg `trace <dll>`: a bit per gDll entry
		// bridge.cfg `traceheap`: only the allocator rows and native virtual calls (a full trace fills the log in ~4 min)
volatile int gLockWord;
struct TLock
{
	TLock() { while (__atomic_exchange_n(&gLockWord, 1, __ATOMIC_ACQUIRE)) {} }
	~TLock() { __atomic_store_n(&gLockWord, 0, __ATOMIC_RELEASE); }
};
enum { KMaxCpus = 128, KGuestStack = 64 * 1024 };
struct TCpuOf { uint32_t iId; CArmCpu* iCpu; };
TCpuOf gCpus[KMaxCpus];
CArmCpu* gPrimary;
uint32_t ThreadId() { return gThr.iThreadId ? gThr.iThreadId() : 0; }
CArmCpu* CpuOfThisThread()
{
	const uint32_t id = ThreadId();
	TLock lock;
	for (int i = 0; i < KMaxCpus; ++i)
		if (gCpus[i].iCpu && gCpus[i].iId == id) return gCpus[i].iCpu;
	return 0;
}
bool SetCpuOfThisThread(CArmCpu* aCpu, CArmCpu* aWas)
{
	const uint32_t id = ThreadId();
	TLock lock;
	for (int i = 0; i < KMaxCpus; ++i)
		if (gCpus[i].iCpu == aWas && (aWas ? gCpus[i].iId == id : true)) { gCpus[i].iId = id; gCpus[i].iCpu = aCpu; return true; }
	return false;
}
}

void BridgeSetThreads(const TBridgeThreads& aThreads) { gThr = aThreads; }
bool BridgeIsGuestThread(const CArmCpu& aCpu) { return gPrimary && &aCpu != gPrimary; }

// the primary CPU of the process (the main thread's): registered by the first bridged call it makes
static void RegisterPrimary(CArmCpu& aCpu)
{
	if (gPrimary) return;
	gPrimary = &aCpu;
	SetCpuOfThisThread(&aCpu, 0);
}

static uint32_t RunOnNewCpu(uint32_t aFn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	if (!gPrimary || !gThr.iAlloc) return 0;
	const uint32_t guestAlloc = HostHeapEnter();		// the new thread's heap (the guest's): Proteus' CPU, stack and translator go elsewhere
	if (gTrace && gThr.iLog) gThr.iLog("[proteus] newcpu enter");
	const uint32_t stack = gThr.iAlloc(KGuestStack);
	if (gTrace && gThr.iLog) gThr.iLog("[proteus] newcpu stack");
	if (!stack) { HostHeapLeave(*gPrimary, false); if (guestAlloc) gThr.iSwitchAlloc(guestAlloc); return 0; }
	CArmCpu* c = new CArmCpu(*gPrimary);				// the memory view (region, page map, windows) and the hooks are shared
	memset(c->R, 0, sizeof(c->R)); memset(c->iD, 0, sizeof(c->iD));
	c->iN = c->iZ = c->iC = c->iV = c->iT = c->iGE = 0;
	c->iFpscr = 0; c->iTpidrurw = c->iTpidruro = 0;
	c->iHalt = ERunning; c->iExitCode = 0; c->iSteps = 0; c->iExcl = false;
	c->iSliceSteps = 0; c->iNextSlice = 0;				// no guest scheduler: the kernel schedules the thread
	c->iStatSlow = c->iStatSvc = c->iStatTrap = c->iStatMem = 0;
	c->iNote[0] = 0;
	c->iGuestAlloc = guestAlloc;
	c->iJit = gThr.iJitNew ? gThr.iJitNew(*c) : 0;
	if (!SetCpuOfThisThread(c, 0)) { if (c->iJit && gThr.iJitDelete) gThr.iJitDelete(c->iJit); delete c; if (gThr.iFree) gThr.iFree(stack); if (guestAlloc) gThr.iSwitchAlloc(guestAlloc); return 0; }
	c->R[0] = a0; c->R[1] = a1; c->R[2] = a2; c->R[3] = a3;
	c->R[13] = (stack + KGuestStack) & ~7u;
	c->R[14] = KReturnToHost;
	c->Branch(aFn);
	if (gTrace && gThr.iLog) gThr.iLog("[proteus] newcpu run");
	NestEnter(*c);
	c->Run(~0ull);
	int32_t leave = 0;
	const bool left = NestLeave(*c, leave);
	if (left) c->iHalt = EReturned;
	const uint32_t result = c->iHalt == EReturned ? c->R[0] : 0;
	if (c->iHalt != EReturned && gThr.iLog)
	{
		char line[200];
		snprintf(line, sizeof(line), "[proteus] guest thread %x stopped: halt %d exit %d pc %x fault %x sp %x note '%s'", ThreadId(), (int)c->iHalt, c->iExitCode, c->iLastPc, c->iFaultAddr, c->R[13], c->iNote);
		gThr.iLog(line);
	}
	SetCpuOfThisThread(0, c);
	if (c->iJit && gThr.iJitDelete) gThr.iJitDelete(c->iJit);
	ForgetLeaveState(*c);
	ForgetGuestTls(*c);
	const uint32_t endAlloc = c->iGuestAlloc;		// the thread ends on the allocator its guest code left it on
	delete c;
	if (gThr.iFree) gThr.iFree(stack);
	if (endAlloc && gThr.iSwitchAlloc) gThr.iSwitchAlloc(endAlloc);
	if (left)
		NativeLeave(leave);					// a guest thread function that left with no TRAP: the native thread leaves (and panics, as natively)
	return result;
}

namespace
{
// Reverse stubs are made at run time: each is a 32-byte piece of x86-64 code in an executable block from the embedder (TBridgeThreads::
// iCodeAlloc; a host test maps its own) that loads its index into the 5th argument register (r8d) and jumps to the dispatcher of its kind,
// which finds the guest function by that index. Blocks of 256 stubs come as needed (round 14; the fixed template table of round 13 held
// 4096 per kind and cost the handler 2 MB of code). The index of a (guest function, kind) pair is kept in an open-addressing hash.
enum { KStubsPerBlock = 256, KStubBytes = 32, KMaxBlocks = 128, KMaxStubs = KStubsPerBlock * KMaxBlocks, KStubHash = 2 * KMaxStubs };
// the kinds of reverse stub, by how the function returns (gen_bridge.py slot_kinds): plain (a word, a 64-bit value or a struct of up to 4
// bytes: the same registers on both sides); `r` a struct of 5-16 bytes the native caller expects in rax:rdx while the ARM function writes it
// through a hidden pointer in r0 ahead of `this`; `m` a struct both sides return through a hidden pointer first (passed on and returned)
enum TStubKind { EStubPlain, EStubRegs, EStubMem, EStubKinds };
struct TRevStub { uint32_t iFn; int iKind; };
struct TStubBlock { uint8_t iCode[KStubsPerBlock * KStubBytes]; TRevStub iStub[KStubsPerBlock]; };
TStubBlock* gStubBlk[KMaxBlocks];
int32_t* gStubHash;		// KStubHash slots: stub index + 1, 0 = empty
int gStubCount;
inline TRevStub& Stub(uint32_t k) { return gStubBlk[k / KStubsPerBlock]->iStub[k % KStubsPerBlock]; }
struct TRet16 { uint32_t iW[4]; };		// an INTEGER-class 16-byte aggregate: rax:rdx (a 8- or 12-byte caller reads the leading words)
uint32_t RevStub(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k)
{
	CArmCpu* cpu = CpuOfThisThread();
	const uint32_t fn = Stub(k).iFn;
	return cpu ? BridgeCallGuest(*cpu, fn, a, b, c, d) : RunOnNewCpu(fn, a, b, c, d);
}
TRet16 RevStubR(uint32_t aThis, uint32_t b, uint32_t c, uint32_t, uint32_t k)
{
	TRet16 r;
	memset(&r, 0, sizeof(r));
	CArmCpu* cpu = CpuOfThisThread();
	const uint32_t fn = Stub(k).iFn;
	if (cpu)
	{	// the result buffer below the guest's sp, above the frame BridgeCallGuest starts at (sp - 64)
		const uint32_t buf = (cpu->R[13] - 32) & ~7u;
		if (cpu->Valid(buf, 16))
		{
			BridgeCallGuest(*cpu, fn, buf, aThis, b, c);
			for (int i = 0; i < 4; ++i) r.iW[i] = cpu->Read32(buf + 4 * i);
		}
	}
	else if (sizeof(void*) == 4)		// a new CPU maps native memory: the native local is the buffer
		RunOnNewCpu(fn, (uint32_t)(uintptr_t)&r, aThis, b, c);
	return r;
}
uint32_t RevStubM(uint32_t aRet, uint32_t aThis, uint32_t b, uint32_t c, uint32_t k)
{
	RevStub(aRet, aThis, b, c, k);
	return aRet;					// (SysV: rax = the hidden result pointer; the AAPCS does not promise r0)
}
const uintptr_t KStubDispatch[EStubKinds] = { (uintptr_t)&RevStub, (uintptr_t)&RevStubR, (uintptr_t)&RevStubM };

// executable memory for a block: the embedder's, or (a host test without one) an anonymous mapping
uint8_t* StubMemory(uint32_t aBytes)
{
	if (gThr.iCodeAlloc) return (uint8_t*)gThr.iCodeAlloc(aBytes);
#if defined(__linux__) && !defined(__SYMBIAN32__)
	void* p = mmap(0, aBytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? 0 : (uint8_t*)p;
#else
	return 0;
#endif
}
// stub k's code: mov r8d, k; mov rax, <dispatcher>; jmp rax
void WriteStub(uint8_t* aAt, uint32_t aK, uintptr_t aTo)
{
	uint8_t* p = aAt;
	*p++ = 0x41; *p++ = 0xB8; memcpy(p, &aK, 4); p += 4;
	if (sizeof(void*) == 4) { const uint32_t to = (uint32_t)aTo; *p++ = 0xB8; memcpy(p, &to, 4); p += 4; }		// mov eax, imm32 (zero-extends)
	else { const uint64_t to = (uint64_t)aTo; *p++ = 0x48; *p++ = 0xB8; memcpy(p, &to, 8); p += 8; }		// movabs rax, imm64
	*p++ = 0xFF; *p++ = 0xE0;
	while (p < aAt + KStubBytes) *p++ = 0xCC;
}
uintptr_t StubCode(uint32_t k) { return (uintptr_t)&gStubBlk[k / KStubsPerBlock]->iCode[(k % KStubsPerBlock) * KStubBytes]; }
}

// the native address of stub k: on x32 its code; on an LP64 host test a token BridgeStubFn turns back into the code
static uint32_t StubAddress(int k) { return sizeof(void*) == 4 ? (uint32_t)StubCode((uint32_t)k) : 0xE0000000u | (uint32_t)k; }
uintptr_t BridgeStubFn(uint32_t aAddr)
{
	if ((aAddr & 0xFF000000u) != 0xE0000000u) return (uintptr_t)aAddr;
	const uint32_t k = aAddr & 0xFFFFFF;
	return k < (uint32_t)__atomic_load_n(&gStubCount, __ATOMIC_ACQUIRE) ? StubCode(k) : 0;
}

// the guest function behind a reverse stub address (0 = not a stub): guest code reached a stub through a patched vtable slot (see PatchVtable)
uint32_t BridgeStubTarget(uint32_t aPc)
{
	const uint32_t n = (uint32_t)__atomic_load_n(&gStubCount, __ATOMIC_ACQUIRE);
	const uint32_t pc = aPc & ~1u;
	if (sizeof(void*) != 4)
		return (pc & 0xFF000000u) == 0xE0000000u && (pc & 0xFFFFFF) < n ? Stub(pc & 0xFFFFFF).iFn : 0;
	for (uint32_t b = 0; b * KStubsPerBlock < n; ++b)
	{
		const uintptr_t at = (uintptr_t)gStubBlk[b]->iCode;
		if (pc >= at && pc < at + sizeof(gStubBlk[b]->iCode) && (pc - at) % KStubBytes == 0)
		{
			const uint32_t k = b * KStubsPerBlock + (uint32_t)(pc - at) / KStubBytes;
			return k < n ? Stub(k).iFn : 0;
		}
	}
	return 0;
}

static uint32_t BrCb(CArmCpu& aCpu, uint32_t aGuestFn, int aKind = EStubPlain)
{
	if (!aGuestFn || !aCpu.InRegion(aGuestFn & ~1u, 2))
		return aGuestFn;
	TLock lock;
	if (!gStubHash) gStubHash = (int32_t*)StubMemory(KStubHash * sizeof(int32_t));		// (zeroed: fresh code-chunk pages / mmap)
	const char* full = !gStubHash ? "bridge: no memory for callback stubs" : 0;
	uint32_t h = (aGuestFn * 2654435761u ^ (uint32_t)aKind) & (KStubHash - 1);
	for (; !full && gStubHash[h]; h = (h + 1) & (KStubHash - 1))
	{
		const TRevStub& s = Stub((uint32_t)gStubHash[h] - 1);
		if (s.iFn == aGuestFn && s.iKind == aKind) return StubAddress(gStubHash[h] - 1);
	}
	const uint32_t k = (uint32_t)gStubCount;
	if (!full && k == KMaxStubs) full = "bridge: out of callback stubs";
	if (!full && k % KStubsPerBlock == 0 && !gStubBlk[k / KStubsPerBlock])
	{
		gStubBlk[k / KStubsPerBlock] = (TStubBlock*)StubMemory(sizeof(TStubBlock));
		if (!gStubBlk[k / KStubsPerBlock]) full = "bridge: no memory for callback stubs";
	}
	if (full) { aCpu.Halt(EHleStop); strncpy(aCpu.iNote, full, sizeof(aCpu.iNote) - 1); return 0; }
	Stub(k).iFn = aGuestFn;
	Stub(k).iKind = aKind;
	WriteStub((uint8_t*)StubCode(k), k, KStubDispatch[aKind]);
	gStubHash[h] = (int32_t)k + 1;
	__atomic_store_n(&gStubCount, (int)k + 1, __ATOMIC_RELEASE);
	return StubAddress((int)k);
}
static void BrCbField(CArmCpu& aCpu, uint8_t* aField)
{
	uint32_t v;
	memcpy(&v, aField, 4);
	v = BrCb(aCpu, v);
	memcpy(aField, &v, 4);
}

#include "gen/bridge_shapes.inc"
#include "gen/bridge_euser.inc"
#include "gen/bridge_efsrv.inc"
#include "gen/bridge_libc.inc"
#include "gen/bridge_libpthread.inc"

namespace
{
struct TBridgeDll
{
	const char* iName;
	const TBridgeExport* iRows;
	uint32_t iCount;
	bool iNativeOn;
	uintptr_t* iFn;			// the native address per row, looked up once (0 = not yet)
	bool* iKeepHle;			// per row: stays with the HLE (process and thread set-up the native process has already done, the exit)
	const TBridgeVtArg* iVt;	// the generated polymorphic-argument list (KBridgeVt_<dll>)
	const TBridgeTi* iTi;		// the ARM type_info objects of its _ZTI exports (KBridgeTi_<dll>)
	int16_t* iVtRow;		// per row: its first entry there, -1 = none
	bool* iLeaves;			// per row: the function's name says it can leave (...L) and returns with a balanced cleanup stack
};
// imports that are not bridged even for a native DLL: the guest EXE's start-up and exit belong to Proteus, not to the native process
const char* const KKeepHle[] = { "_ZN4User11InitProcessEv", "_ZN7UserSvr9DllSetTlsEiiPv", "_ZN4User4ExitEi", "_ZN6RDebug6PrintfEPKcz" };
// euser and efsrv first (code below indexes them as 0 and 1); Open C's libc and libpthread since round 16 (no ARM C library beside a native one)
const int KBridgeDlls = 4;
TBridgeDll gDll[KBridgeDlls] = { { "euser.dll", KBridge_euser, 0, false, 0, 0, KBridgeVt_euser, KBridgeTi_euser, 0, 0 },
	{ "efsrv.dll", KBridge_efsrv, 0, false, 0, 0, KBridgeVt_efsrv, KBridgeTi_efsrv, 0, 0 },
	{ "libc.dll", KBridge_libc, 0, false, 0, 0, KBridgeVt_libc, KBridgeTi_libc, 0, 0 },
	{ "libpthread.dll", KBridge_libpthread, 0, false, 0, 0, KBridgeVt_libpthread, KBridgeTi_libpthread, 0, 0 } };
uint32_t gLo = 0x00010000, gHi = 0xC0000000;
bool gMap;
uintptr_t (*gResolve)(uint32_t);	// host tests: the "native address" the embedder returns is a 32-bit token, the function pointer is 64-bit

bool SameDll(const char* aImport, const char* aDll)
{
	for (; *aDll; ++aImport, ++aDll)
	{
		if (*aImport == '{' || *aImport == '[')
			while (*aImport && *aImport != '.') ++aImport;
		char a = *aImport, b = *aDll;
		if (a >= 'A' && a <= 'Z') a += 32;
		if (b >= 'A' && b <= 'Z') b += 32;
		if (a != b) return false;
	}
	return *aImport == 0;
}
}

// the unqualified function name of an Itanium-mangled symbol ends in L (Symbian: the function may leave)
static bool NameLeaves(const char* m)
{
	if (strncmp(m, "_Z", 2)) return false;
	m += 2;
	if (*m == 'N') { ++m; while (*m == 'K' || *m == 'V' || *m == 'r') ++m; }
	const char* last = 0; int lastLen = 0;
	while (*m >= '0' && *m <= '9')
	{
		int n = 0;
		while (*m >= '0' && *m <= '9') n = n * 10 + (*m++ - '0');
		last = m; lastLen = n;
		for (int i = 0; i < n && *m; ++i) ++m;
	}
	if (!last || lastLen < 2) return false;
	if (lastLen >= 5 && !strncmp(last + lastLen - 5, "PushL", 5)) return false;	// (CleanupStack::PushL: returns with the item pushed, like an LC)
	const char c1 = last[lastLen - 1], c2 = last[lastLen - 2];
	return c1 == 'L' && ((c2 >= 'a' && c2 <= 'z') || (c2 >= '0' && c2 <= '9'));
	// (not ...LC / ...LD: they return with an item on the cleanup stack, which a TRAP around the call would panic on -- CBase 71)
}
namespace { struct TTrappedCall { CArmCpu* iCpu; void (*iCall)(CArmCpu&, uintptr_t); uintptr_t iFn; static void Run(void* a) { TTrappedCall& t = *(TTrappedCall*)a; t.iCall(*t.iCpu, t.iFn); } }; }

void BridgeConfigure(const char* aText)
{
	gDll[0].iCount = KBridge_euser_Count;
	gDll[1].iCount = KBridge_efsrv_Count;
	gDll[2].iCount = KBridge_libc_Count;
	gDll[3].iCount = KBridge_libpthread_Count;
	for (int i = 0; i < KBridgeDlls; ++i)
		gDll[i].iNativeOn = false;
	gTrace = false;
	gTraceHeap = false;
	gTraceDll = 0;
	gTraceOnLoad[0] = 0;
	gMap = false;
	char line[160];
	while (aText && *aText)
	{
		int n = 0;
		while (*aText && *aText != '\n' && n < (int)sizeof(line) - 1) { if (*aText != '\r') line[n++] = *aText; ++aText; }
		while (*aText && *aText != '\n') ++aText;
		if (*aText == '\n') ++aText;
		line[n] = 0;
		char* hash = strchr(line, '#');
		if (hash) *hash = 0;
		char w[32], a[64];
		unsigned long x = 0, y = 0;
		a[0] = 0;
		if (sscanf(line, "%31s %63s", w, a) < 1) continue;
		if (!strcmp(w, "native"))
			for (int i = 0; i < KBridgeDlls; ++i) { if (SameDll(a, gDll[i].iName)) gDll[i].iNativeOn = true; }
		else if (!strcmp(w, "trace") && a[0])		// `trace <dll>`: only that DLL's rows (a full trace of libc's getc loop is too slow)
			{ for (int i = 0; i < KBridgeDlls; ++i) if (SameDll(a, gDll[i].iName)) gTraceDll |= 1u << i; }
		else if (!strcmp(w, "trace"))
			gTrace = true;
		else if (!strcmp(w, "traceonload"))
			snprintf(gTraceOnLoad, sizeof(gTraceOnLoad), "%s", a[0] ? a : ".");
		else if (!strcmp(w, "traceheap"))
			gTraceHeap = true;
		else if (!strcmp(w, "watch"))
			gWatch = (uint32_t)strtoul(a, 0, 16);
		else if (!strcmp(w, "watchsize"))
			gWatchSize = (uint32_t)strtoul(a, 0, 16);
		else if (!strcmp(w, "map") && sscanf(line, "%*s %lx %lx", &x, &y) == 2)
			{ gLo = (uint32_t)x; gHi = (uint32_t)y; gMap = true; }
		else if (!strcmp(w, "map"))
			gMap = true;
	}
	// a native DLL implies the map: its objects (the synthesized type_infos, CBase objects, heap cells) live at native addresses the guest
	// must reach (without it PyS60's estor.dll entry faulted 'write8' on a type_info in the native heap)
	if (gDll[0].iNativeOn || gDll[1].iNativeOn || gDll[2].iNativeOn || gDll[3].iNativeOn)
		gMap = true;
	for (int i = 0; i < KBridgeDlls; ++i)
	{
		TBridgeDll& d = gDll[i];
		free(d.iFn); free(d.iKeepHle);
		d.iFn = (uintptr_t*)calloc(d.iCount, sizeof(uintptr_t));
		d.iKeepHle = (bool*)calloc(d.iCount, sizeof(bool));
		free(d.iLeaves);
		d.iLeaves = (bool*)calloc(d.iCount, sizeof(bool));
		for (uint32_t r = 0; d.iLeaves && r < d.iCount; ++r)
			d.iLeaves[r] = NameLeaves(d.iRows[r].iName);
		free(d.iVtRow);
		d.iVtRow = (int16_t*)malloc(d.iCount * sizeof(int16_t));
		for (uint32_t r = 0; d.iVtRow && r < d.iCount; ++r)
		{
			d.iVtRow[r] = -1;
			for (int k = 0; d.iVt[k].iName; ++k)
				if (!strcmp(d.iVt[k].iName, d.iRows[r].iName)) { d.iVtRow[r] = (int16_t)k; break; }
		}
		for (uint32_t r = 0; r < d.iCount; ++r)
			for (unsigned k = 0; k < sizeof(KKeepHle) / sizeof(KKeepHle[0]); ++k)
				if (!strcmp(d.iRows[r].iName, KKeepHle[k])) d.iKeepHle[r] = true;
			// (UserHeap::SetupThreadHeap* is a prefix match: its mangled name carries the create-info type)
		for (uint32_t r = 0; r < d.iCount; ++r)
			if (!strncmp(d.iRows[r].iName, "_ZN8UserHeap15SetupThreadHeap", 29)) d.iKeepHle[r] = true;
	}
}

void BridgeSetResolver(uintptr_t (*aFn)(uint32_t)) { gResolve = aFn; }

// the embedder's native export lookup for the loader, before any guest runs (the loader has no THle): by DLL name and native ordinal
static uint32_t (*gLookup)(void* aCtx, const char* aDll, int aOrdinal);
static void* gLookupCtx;
void BridgeSetLookup(uint32_t (*aFn)(void*, const char*, int), void* aCtx) { gLookup = aFn; gLookupCtx = aCtx; }

// an import of DATA (a vtable, a type_info, an exported variable) of a native DLL: the guest reads it as a word / an object, so the import word
// must be the native address itself, not a trap. Returns false = not data, or not native: the import stays a trap slot.
bool BridgeDataImport(const char* aDll, uint32_t aOrd, uint32_t& aAddr)
{
	for (int i = 0; i < KBridgeDlls; ++i)
	{
		if (!gDll[i].iNativeOn || !SameDll(aDll, gDll[i].iName) || aOrd < 1 || aOrd > gDll[i].iCount) continue;
		const TBridgeExport& e = gDll[i].iRows[aOrd - 1];
		if (!(e.iFlags & (BR_DATA | BR_VTABLE | BR_TYPEINFO)) || !e.iNative || !gLookup) return false;
		aAddr = gLookup(gLookupCtx, gDll[i].iName, e.iNative);
		return aAddr != 0;
	}
	return false;
}

bool BridgeNative(const char* aImport)
{
	for (int i = 0; i < KBridgeDlls; ++i)
		if (gDll[i].iNativeOn && SameDll(aImport, gDll[i].iName)) return true;
	return false;
}

bool BridgeMapWanted(uint32_t& aLo, uint32_t& aHi)
{
	aLo = gLo;
	aHi = gHi;
	return gMap;
}

static THle* gCurHle;		// the guest being served (the hand-written handlers look natives up through it)

// the native address of row aRow of aDll (looked up once); 0 = the native DLL does not export it
static uintptr_t NativeOf(TBridgeDll* d, uint32_t aRow, THle& aHle)
{
	const TBridgeExport& e = d->iRows[aRow];
	if (!d->iFn[aRow] && e.iNative && aHle.iIpc && aHle.iIpc->iNativeExport)
	{
		d->iFn[aRow] = aHle.iIpc->iNativeExport(aHle.iIpc->iCtx, d->iName, e.iNative);
		if (d->iFn[aRow] && gResolve)
			d->iFn[aRow] = gResolve((uint32_t)d->iFn[aRow]);
		static bool once[KBridgeDlls];
		const int di = (int)(d - gDll);
		if (d->iFn[aRow] && di >= 0 && di < KBridgeDlls && !once[di])
		{	// one line per DLL: where its code and the handler's are (maps an EUSRNIMP reason / a fault eip to a module)
			once[di] = true;
			char line[160];
			snprintf(line, sizeof(line), "[proteus] native %s first row %s at %x (handler BridgeTrap at %x)", d->iName, e.iName,
			         (unsigned)d->iFn[aRow], (unsigned)(uintptr_t)&BridgeTrap);
			Say(aHle, "%s", line);
		}
	}
	return d->iFn[aRow];
}

// a native export of euser by its (ARM-spelled) name -- for a handler that calls a different function than the one imported
static uintptr_t NativeByName(const char* aName)
{
	TBridgeDll* d = &gDll[0];
	for (uint32_t r = 0; r < d->iCount; ++r)
		if (!strcmp(d->iRows[r].iName, aName)) return gCurHle ? NativeOf(d, r, *gCurHle) : 0;
	return 0;
}

// ---- ARM-derived objects called from native code (the plan's shadow vtables). An ARM class that derives from a native one (a CActive with its
// own RunL, a CBase the native cleanup stack destroys, an own CActiveScheduler with Error) has an ARM vtable whose slots hold ARM code addresses
// native code cannot call. When such an object crosses into native code (the exports below take one), the base class's slots of its vtable are
// patched IN PLACE to reverse stubs (the vtable is in the guest's own image, which the host can write). In place, not a copy: the ARM ctors and
// dtors store the vtable's address again, and every object of the class shares it. The ARM side then calls those slots through the stubs too: the
// fetch outside the guest region maps a stub back to its ARM function (BridgeStubTarget). Slots the derived class adds stay ARM (only ARM calls
// them); a slot that already holds a native address (an inherited native implementation) stays as it is. Slot counts from the class records
// (tools/bridge/abi/<dll>.json): gen_bridge.py lists every pointer/reference argument of a class with a vtable (KBridgeVt_<dll>).
static void PatchVtable(CArmCpu& aCpu, uint32_t aObj, int aSlots, const char* aKinds)
{
	if (!aObj || !aCpu.Valid(aObj, 4)) return;			// (the object may be on the native heap: a window, not the region)
	const uint32_t vt = aCpu.Read32(aObj);
	if (!aCpu.InRegion(vt, 4u * aSlots)) return;			// a native vtable (a native-created object): nothing to do
	for (int k = 0; k < aSlots; ++k)
	{
		const uint32_t f = aCpu.Read32(vt + 4 * k);
		if (f && aCpu.InRegion(f & ~1u, 2))
			aCpu.Write32(vt + 4 * k, BrCb(aCpu, f, !aKinds || !aKinds[k] || aKinds[k] == '.' ? EStubPlain : aKinds[k] == 'r' ? EStubRegs : EStubMem));
	}
}

static void PatchArgVtables(CArmCpu& aCpu, const TBridgeDll& aDll, uint32_t aRow)
{
	const int16_t k0 = aDll.iVtRow ? aDll.iVtRow[aRow] : -1;
	if (k0 < 0) return;
	BrArgs a(aCpu);
	for (int k = k0; aDll.iVt[k].iName && !strcmp(aDll.iVt[k].iName, aDll.iRows[aRow].iName); ++k)
	{
		const uint32_t obj = a.Raw(aDll.iVt[k].iWord);
		if (aDll.iVt[k].iSlots >= 0)
			PatchVtable(aCpu, obj, aDll.iVt[k].iSlots, aDll.iVt[k].iKinds);
		else if (obj && aCpu.Valid(obj - 1 - aDll.iVt[k].iSlots, 4))		// a TCallBack / TCleanupItem by reference: its function becomes a stub in place
		{
			const uint32_t at = obj - 1 - aDll.iVt[k].iSlots;
			aCpu.Write32(at, BrCb(aCpu, aCpu.Read32(at)));
		}
	}
}

// the watch (bridge.cfg `watch`) also covers the word before it (the previous chunk's footer / prev_size) and is checked around every
// native-call too: a change seen at one names the code that ran since the last check
static uint32_t BridgeNativeCallBody(CArmCpu& aCpu, uint32_t aPc);
static bool NearImage(uint32_t aVt, uint32_t aPc) { return aVt && !(aVt & 3) && (aVt > aPc ? aVt - aPc : aPc - aVt) < 0x1000000u; }
static void WatchCheck(CArmCpu& aCpu, const char* aWhen, uint32_t aPc)
{
	if (!gWatchOn || !gCurHle) return;
	const uint32_t v = aCpu.Read32(gWatch), p = aCpu.Read32(gWatch - 4);
	if (v == gWatchVal && p == gWatchPrev) return;
	char line[160];
	snprintf(line, sizeof(line), "[proteus] watch %x: [%x %x] -> [%x %x] %s native-call %x (lr %x)", gWatch, gWatchPrev, gWatchVal, p, v, aWhen, aPc,
	         aCpu.R[14]);
	Say(*gCurHle, "%s", line);
	gWatchVal = v; gWatchPrev = p;
}

uint32_t BridgeNativeCall(CArmCpu& aCpu, uint32_t aPc)
{
	const uint32_t r = BridgeNativeCallBody(aCpu, aPc);
	WatchCheck(aCpu, "after", aPc);
	return r;
}

static uint32_t BridgeNativeCallBody(CArmCpu& aCpu, uint32_t aPc)
{
	if (!gDll[0].iNativeOn) return 0;
	const uintptr_t fn = gResolve ? gResolve(aPc) : (uintptr_t)aPc;
	if (!fn) return 0;
	WatchCheck(aCpu, "before", aPc);
	TGuestHeap heap(aCpu, false);
	if ((gTrace || gTraceHeap) && gCurHle)
	{
		char line[120];
		snprintf(line, sizeof(line), "[proteus] bridge native-call %x (%x,%x,%x,%x) lr %x", aPc, aCpu.R[0], aCpu.R[1], aCpu.R[2], aCpu.R[3], aCpu.R[14]);
		Say(*gCurHle, "%s", line);
	}
	{	// a native virtual returning a struct through the AAPCS hidden pointer: r0 = the result buffer, r1 = `this`. Found by the slot of r1's vtable
		// that holds aPc (KBridgeVret_<dll>, from the class records); the slot's generated shape makes the x32 call (rax:rdx or its own hidden
		// pointer). r0 must not be an object whose vtable holds aPc at that slot too (then r0 is `this` and r1 an argument of the same class)
		const uint32_t self = aCpu.R[1], res = aCpu.R[0];
		// a candidate vtable is read only when it lies in the image of aPc (a native virtual's vtable is in the DLL that holds the function):
		// Valid() is true for any address of the native window, committed or not, and r1 is often no object at all -- Free(heap, cell) passed
		// a freed stdio buffer whose first word (file text) was probed as a vtable and faulted on an uncommitted page (round 16)
		uint32_t vt = self && aCpu.Valid(self, 4) ? aCpu.Read32(self) : 0;
		uint32_t vt0 = res && aCpu.Valid(res, 4) ? aCpu.Read32(res) : 0;
		if (!NearImage(vt, aPc)) vt = 0;
		if (!NearImage(vt0, aPc)) vt0 = 0;
		const TBridgeVret* const tabs[] = { KBridgeVret_euser, KBridgeVret_efsrv, KBridgeVret_libc, KBridgeVret_libpthread };
		for (const TBridgeVret* t : tabs)
			for (; vt && t->iCall; ++t)
				if (aCpu.Valid(vt + 4 * t->iSlot, 4) && aCpu.Read32(vt + 4 * t->iSlot) == aPc &&
				    !(vt0 && aCpu.Valid(vt0 + 4 * t->iSlot, 4) && aCpu.Read32(vt0 + 4 * t->iSlot) == aPc))
				{
					if (gTrace && gCurHle)
					{
						char line[160];
						snprintf(line, sizeof(line), "[proteus] bridge native-call %x: slot %d returns a struct (%s)", aPc, t->iSlot, t->iDecl);
						Say(*gCurHle, "%s", line);
					}
					t->iCall(aCpu, fn);
					return aCpu.R[14] ? aCpu.R[14] : 0;
				}
	}
	const uint32_t sp = aCpu.R[13];
	const uint32_t s0 = aCpu.Valid(sp, 4) ? aCpu.Read32(sp) : 0, s1 = aCpu.Valid(sp + 4, 4) ? aCpu.Read32(sp + 4) : 0;
	typedef uint64_t (*F)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
	const uint64_t r = ((F)fn)(aCpu.R[0], aCpu.R[1], aCpu.R[2], aCpu.R[3], s0, s1);
	aCpu.R[0] = (uint32_t)r; aCpu.R[1] = (uint32_t)(r >> 32);
	return aCpu.R[14] ? aCpu.R[14] : 0;
}

uint32_t BridgeOnForeignPc(CArmCpu& aCpu, uint32_t aPc, void*)
{
	const uint32_t t = BridgeStubTarget(aPc);
	return t ? t : BridgeNativeCall(aCpu, aPc);
}

// ---- leaves (R1). The ARM side is leave-as-throw: an ARM TRAP is User::MarkCleanupStack() + try/catch (XLeaveException&), and the unwinder,
// __cxa_throw and XLeaveException's vtable/type_info live in the guest's ARM drtaeabi.dll. The native euser is the setjmp TRAP model. So, as
// toolchain/euser_dll/dev_markcleanup.cpp does for the device euser, the bridge keeps a MARKER per ARM MarkCleanupStack (r4-r11, sp, lr at the
// call, the native trap handler, the nested-run depth) and a guest User::Leave* becomes:
//   * a marker at the current nested-run depth: the native cleanup stack is unwound to the level Mark opened (the handler's Leave), then the
//     guest resumes AS IF drtaeabi's __cxa_throw(XLeaveException(reason)) had been called from the Mark call site -- its unwinder walks the
//     ARM frames to the catch. The catch clause's _ZTI15XLeaveException import of euser is aliased to drtaeabi's (BridgeDataAlias), so the
//     thrown and the caught type_info are one object.
//   * no marker at this depth, inside a nested run (an ARM RunL called by the native scheduler, a callback): the nested run stops, the CPU is
//     restored (BridgeCallGuest) and the native User::Leave is raised from the stub's own native frame, to the native TRAP around the call.
//   * no marker at all on the primary: the native User::Leave (no trap: the panic the native euser gives).
// Not yet: a NATIVE callee that leaves (a bridged ...L export failing) longjmps over the bridge frames to the nearest native TTrap.
namespace
{
enum { KMaxMarks = 64, KMaxLeaveCpus = 128 };
struct TMark { uint32_t iCtx[10]; uint32_t iHandler; int iDepth; uint32_t iResult; };	// iResult: a TTrap::Trap marker (the TInt& to set), 0 = Mark
struct TLeaveState { CArmCpu* iCpu; int iDepth; int iMarks; TMark iMark[KMaxMarks]; bool iPending; int32_t iReason; };
TLeaveState gLeave[KMaxLeaveCpus];
TLeaveState* LeaveState(CArmCpu& aCpu)
{
	TLock lock;
	for (int i = 0; i < KMaxLeaveCpus; ++i) if (gLeave[i].iCpu == &aCpu) return &gLeave[i];
	for (int i = 0; i < KMaxLeaveCpus; ++i) if (!gLeave[i].iCpu) { memset(&gLeave[i], 0, sizeof(gLeave[i])); gLeave[i].iCpu = &aCpu; return &gLeave[i]; }
	return 0;
}
// drtaeabiu.def ordinals (dev_markcleanup.cpp): __cxa_allocate_exception, __cxa_throw, XLeaveException type_info / vtable, the exception globals
enum { KOrdAllocEx = 177, KOrdThrow = 191, KOrdTypeInfo = 206, KOrdVtable = 207, KOrdCtorGlobals = 204, KOrdGetGlobals = 216, KOrdInit2 = 226 };
uint32_t DrtExport(uint32_t aOrd)
{
	if (!gCurHle || !gCurHle->iProcess || !gPrimary) return 0;
	const TProcess& p = *gCurHle->iProcess;
	for (int m = 1; m < p.iCount; ++m)
		if (SameDll(p.iMod[m].iName, "drtaeabi.dll") && aOrd <= p.iMod[m].iExportCount)
			return gPrimary->Read32(p.iMod[m].iExportDir + 4 * (aOrd - 1));
	return 0;
}
typedef void (*TNatLeave)(int32_t);
}

static void ForgetLeaveState(CArmCpu& aCpu)
{
	TLock lock;
	for (int i = 0; i < KMaxLeaveCpus; ++i) if (gLeave[i].iCpu == &aCpu) gLeave[i].iCpu = 0;
}
// the nested-run depth of a CPU (BridgeCallGuest) and the leave a nested run stopped with
static void NestEnter(CArmCpu& aCpu) { if (TLeaveState* s = LeaveState(aCpu)) ++s->iDepth; }
static bool NestLeave(CArmCpu& aCpu, int32_t& aReason)
{
	TLeaveState* s = LeaveState(aCpu);
	if (!s) return false;
	--s->iDepth;
	while (s->iMarks && s->iMark[s->iMarks - 1].iDepth > s->iDepth) --s->iMarks;		// (markers of the abandoned frames)
	if (!s->iPending) return false;
	s->iPending = false;
	aReason = s->iReason;
	return true;
}
static void NativeLeave(int32_t aReason)
{
	const uintptr_t f = NativeByName("_ZN4User5LeaveEi");
	if (f) ((TNatLeave)f)(aReason);
}

static void GuestLeave(CArmCpu& aCpu, int32_t aReason)
{
	TLeaveState* s = LeaveState(aCpu);
	if (gTrace && gCurHle)
	{
		char line[120];
		snprintf(line, sizeof(line), "[proteus] bridge leave %d depth %d marks %d lr %x", aReason, s ? s->iDepth : -1, s ? s->iMarks : -1, aCpu.R[14]);
		Say(*gCurHle, "%s", line);
	}
	if (s && s->iMarks && s->iMark[s->iMarks - 1].iDepth == s->iDepth)
	{
		const TMark m = s->iMark[--s->iMarks];
		if (m.iHandler && gThr.iTrapLeave)
			gThr.iTrapLeave(m.iHandler, aReason);
		else if (m.iHandler && sizeof(void*) == 4)
		{	// TTrapHandler: Trap, UnTrap, Leave -- slot 2 (no virtual destructor)
			const uint32_t vt = *(const uint32_t*)(uintptr_t)m.iHandler;
			typedef void (*TLeaveFn)(uint32_t, int32_t);
			((TLeaveFn)(uintptr_t)((const uint32_t*)(uintptr_t)vt)[2])(m.iHandler, aReason);
		}
		if (m.iResult)
		{	// a TTrap::Trap marker (the setjmp model of OUR ARM euser, its ordinals 2548/2549): Trap returns again, 1, with the reason set
			aCpu.Write32(m.iResult, (uint32_t)aReason);
			for (int i = 0; i < 8; ++i) aCpu.R[4 + i] = m.iCtx[i];
			aCpu.R[13] = m.iCtx[8];
			aCpu.R[14] = m.iCtx[9];
			aCpu.R[0] = 1;
			aCpu.SetReturn();
			return;
		}
		const uint32_t alloc = DrtExport(KOrdAllocEx), thr = DrtExport(KOrdThrow), ti = DrtExport(KOrdTypeInfo), vt = DrtExport(KOrdVtable);
		if (!alloc || !thr || !ti || !vt)
		{
			strncpy(aCpu.iNote, "bridge: a leave needs the guest's drtaeabi.dll", sizeof(aCpu.iNote) - 1);
			aCpu.Halt(EHleStop);
			return;
		}
		if (!BridgeCallGuest(aCpu, DrtExport(KOrdGetGlobals), 0, 0, 0, 0) && gThr.iAlloc)
		{	// a thread with no exception globals yet (dev_markcleanup.cpp): give it drtaeabi's
			const uint32_t g = gThr.iAlloc(1024);
			BridgeCallGuest(aCpu, DrtExport(KOrdCtorGlobals), g, 0, 0, 0);
			BridgeCallGuest(aCpu, DrtExport(KOrdInit2), g, 0, 0, 0);
		}
		const uint32_t ex = BridgeCallGuest(aCpu, alloc, 8, 0, 0, 0);
		aCpu.Write32(ex, vt + 8);
		aCpu.Write32(ex + 4, (uint32_t)aReason);
		for (int i = 0; i < 8; ++i) aCpu.R[4 + i] = m.iCtx[i];
		aCpu.R[13] = m.iCtx[8];
		aCpu.R[14] = m.iCtx[9];
		aCpu.R[0] = ex; aCpu.R[1] = ti; aCpu.R[2] = 0;
		aCpu.Branch(thr);
		return;
	}
	if (s && (s->iDepth > 0 || BridgeIsGuestThread(aCpu)))
	{	// out of this nested run / guest thread: the native side raises it (BridgeCallGuest, RunOnNewCpu)
		s->iPending = true;
		s->iReason = aReason;
		snprintf(aCpu.iNote, sizeof(aCpu.iNote), "leave %d", aReason);
		aCpu.Halt(EHleStop);
		return;
	}
	NativeLeave(aReason);
}

// per guest CPU (thread) and DLL handle: the guest DLLs' Dll::Tls
namespace { struct TGuestTls { CArmCpu* iCpu; uint32_t iHandle, iValue; }; enum { KMaxTls = 256 }; TGuestTls gTls[KMaxTls]; }
static uint32_t GuestTls(CArmCpu& aCpu, uint32_t aHandle, bool aSet, uint32_t aValue)
{
	TLock lock;
	int freeAt = -1;
	for (int i = 0; i < KMaxTls; ++i)
	{
		if (gTls[i].iCpu == &aCpu && gTls[i].iHandle == aHandle) { if (aSet) gTls[i].iValue = aValue; return gTls[i].iValue; }
		if (!gTls[i].iCpu && freeAt < 0) freeAt = i;
	}
	if (aSet && freeAt >= 0) { gTls[freeAt].iCpu = &aCpu; gTls[freeAt].iHandle = aHandle; gTls[freeAt].iValue = aValue; }
	return aSet ? aValue : 0;
}
static void ForgetGuestTls(CArmCpu& aCpu)
{
	TLock lock;
	for (int i = 0; i < KMaxTls; ++i) if (gTls[i].iCpu == &aCpu) gTls[i].iCpu = 0;
}
static bool IsLeaveRow(const char* n)
{
	static const char* const K[] = { "_ZN4User16MarkCleanupStackEv", "_ZN4User18UnMarkCleanupStackEP12TTrapHandler", "_ZNK15XLeaveException9GetReasonEv",
		"_ZN4User5LeaveEi", "_ZN4User12LeaveIfErrorEi", "_ZN4User13LeaveNoMemoryEv", "_ZN4User11LeaveIfNullEPv",
		"_ZN7UserSvr19ExceptionDescriptorEm", "_ZN7UserSvr6DllTlsEi", "_ZN7UserSvr6DllTlsEii", "_ZN7UserSvr9DllSetTlsEiPv", "_ZN7UserSvr9DllSetTlsEiiPv",
		"_ZN7UserSvr10DllFreeTlsEi" };
	for (const char* k : K) if (!strcmp(n, k)) return true;
	return false;
}

// ---- RLibrary in the guest: the guest's run-time loads (the CDL engine's plug-ins, PyS60's .pyd, ECOM) are ARM modules of THIS process,
// not native DLLs. Bridged to the native euser, RLibrary::Load loaded the native x32 DLL of the same name and Lookup handed the guest a
// native function, which then ran outside the guest (PyS60 on the CI image: `native call 82d0d440 -> no ARM twin`, then CdlEng 0).
// Load = the guest loader (LoadLibraryModule) plus the new modules' entry points in import order; the handle is KHandleLib0 + module.
static bool IsLibraryRow(const char* n)
{
	return !strncmp(n, "_ZN8RLibrary4LoadE", 18) || !strcmp(n, "_ZNK8RLibrary6LookupEi") || !strcmp(n, "_ZN8RLibrary5CloseEv")
		|| !strcmp(n, "_ZN11RHandleBase5CloseEv") || !strcmp(n, "_ZN11RHandleBase9DuplicateERK7RThread10TOwnerType");
}
// the rows that only look at the handle (any guest thread may close or duplicate a library handle; Load/Lookup need the primary's process)
static bool IsLibraryHandleRow(const char* n) { return !strncmp(n, "_ZN11RHandleBase", 16) || !strcmp(n, "_ZN8RLibrary5CloseEv"); }
// a guest RLibrary of a DLL that runs natively (round 16: dlopen("libc.dll") with no ARM libc left): the handle names the bridge table
// (KHandleLib0 + KNativeLib + its index) and Lookup(ord) hands out an import slot of module KBridgeModule, trapped like a linked import
static const uint32_t KNativeLib = 0x8000, KBridgeModule = 0x3FE;
static uint32_t* gLookupSlot[KBridgeDlls];		// per DLL and ordinal: the slot address handed out (0 = none yet)
static uint32_t NativeLookupSlot(TProcess& aProc, int aDll, uint32_t aOrd)
{
	TBridgeDll& d = gDll[aDll];
	if (aOrd < 1 || aOrd > d.iCount) return 0;
	if (!gLookupSlot[aDll]) gLookupSlot[aDll] = (uint32_t*)calloc(d.iCount, sizeof(uint32_t));
	if (!gLookupSlot[aDll]) return 0;
	if (gLookupSlot[aDll][aOrd - 1]) return gLookupSlot[aDll][aOrd - 1];
	if (aProc.iSlotCount == aProc.iSlotMax)
	{
		const uint32_t nmax = aProc.iSlotMax ? aProc.iSlotMax * 2 : 1024;
		uint32_t* ns = (uint32_t*)realloc(aProc.iSlots, nmax * sizeof(uint32_t));
		if (!ns) return 0;
		aProc.iSlots = ns;
		aProc.iSlotMax = nmax;
	}
	const uint32_t slot = aProc.iSlotCount++;
	aProc.iSlots[slot] = (KBridgeModule << 22) | ((uint32_t)aDll << 16) | aOrd;
	return gLookupSlot[aDll][aOrd - 1] = KTrapBase | (slot << 2);
}

static bool LibraryOverride(CArmCpu& aCpu, const char* aName)
{
	TProcess* p = gCurHle ? gCurHle->iProcess : 0;
	const bool lib = (aCpu.R[0] && (aCpu.Read32(aCpu.R[0]) & 0xFFFF0000u) == KHandleLib0);
	if (!strcmp(aName, "_ZN11RHandleBase5CloseEv") || !strcmp(aName, "_ZN8RLibrary5CloseEv"))
	{
		if (!lib) return false;					// any other handle: the native Close
		aCpu.Write32(aCpu.R[0], 0);
		aCpu.SetReturn();
		return true;
	}
	if (!strcmp(aName, "_ZN11RHandleBase9DuplicateERK7RThread10TOwnerType"))
	{	// a guest library handle stays what it is: the module is loaded for the whole process (libdl's dlopen duplicates it, round 14)
		if (!lib) return false;
		aCpu.R[0] = 0;
		aCpu.SetReturn();
		return true;
	}
	if (!p) return false;
	if (!strcmp(aName, "_ZNK8RLibrary6LookupEi"))
	{
		if (!lib) return false;
		const int mod = (int)(aCpu.Read32(aCpu.R[0]) & 0xFFFF);
		const uint32_t ord = aCpu.R[1];
		if (mod >= (int)KNativeLib)
		{
			aCpu.R[0] = mod - (int)KNativeLib < KBridgeDlls ? NativeLookupSlot(*p, mod - (int)KNativeLib, ord) : 0;
			aCpu.SetReturn();
			return true;
		}
		const TLoaded* m = mod < p->iCount ? &p->iMod[mod] : 0;
		aCpu.R[0] = m && ord >= 1 && ord <= m->iExportCount ? aCpu.Read32(m->iExportDir + 4 * (ord - 1)) : 0;
		aCpu.SetReturn();
		return true;
	}
	// Load(const TDesC16& aFileName, ...): the file's base name (a path or drive in front is the guest's search hint; our loader searches)
	char file[160];
	const uint32_t fd = aCpu.R[1], w = aCpu.Read32(fd), type = w >> 28, len = w & 0x0FFFFFFF;
	const uint32_t ptr = type == 0 ? fd + 4 : type == 1 ? aCpu.Read32(fd + 4) : type == 2 ? aCpu.Read32(fd + 8) : type == 3 ? fd + 8 : aCpu.Read32(fd + 8) + 4;
	uint32_t k = 0;
	for (; k < len && k < sizeof(file) - 5; ++k) file[k] = (char)aCpu.Read16(ptr + 2 * k);
	file[k] = 0;
	const char* bs = strrchr(file, 0x5C);
	const char* base = bs ? bs + 1 : strchr(file, ':') ? strchr(file, ':') + 1 : file;
	char name[160];
	snprintf(name, sizeof(name), "%s%s", base, strchr(base, '.') ? "" : ".dll");
	if (gTraceOnLoad[0] && strstr(name, gTraceOnLoad)) gTrace = true;
	for (int i = 0; i < KBridgeDlls; ++i)
		if (gDll[i].iNativeOn && SameDll(name, gDll[i].iName))
		{
			aCpu.Write32(aCpu.R[0], KHandleLib0 + KNativeLib + (uint32_t)i);
			aCpu.R[0] = 0;
			aCpu.SetReturn();
			return true;
		}
	int firstNew = 0;
	const int mod = LoadLibraryModule(aCpu, *p, name, gCurHle->iFind, gCurHle->iFindCtx, firstNew);
	int32_t res = mod >= 0 ? 0 : mod;
	if (mod >= 0 && firstNew < p->iCount)
	{	// the new modules' entry points (KModuleEntryReasonProcessAttach), each after the DLLs it imports
		int order[KMaxModules];
		const int n = InitOrder(*p, mod, firstNew, order);
		for (int i = 0; i < n && !res; ++i)
			if (CallsEntry(aCpu, p->iMod[order[i]]))
				res = (int32_t)BridgeCallGuest(aCpu, p->iMod[order[i]].iEntry, 2, 0, 0, 0);
	}
	if (gTrace || mod < 0)
	{
		char line[220];
		snprintf(line, sizeof(line), "[proteus] bridge RLibrary::Load %s -> %d (%d)", name, mod, (int)res);
		Say(*gCurHle, "%s", line);
	}
	if (!res) aCpu.Write32(aCpu.R[0], KHandleLib0 + (uint32_t)mod);
	aCpu.R[0] = (uint32_t)res;
	aCpu.SetReturn();
	return true;
}
// the hand-written euser rows of the leave model; true = handled (the guest continues at lr, or was redirected)
static bool LeaveOverride(CArmCpu& aCpu, const char* aName, uintptr_t aNative)
{
	const int32_t r0 = (int32_t)aCpu.R[0];
	if (!strncmp(aName, "_ZN7UserSvr", 11))
	{	// the guest's ARM modules are unknown to the native kernel: their unwind tables (the code address -> the module's exception descriptor,
		// scodeseg.cpp) and their per-DLL TLS (keyed by the handle our loader gave the module) are answered here, per guest CPU (= thread)
		if (!strcmp(aName, "_ZN7UserSvr19ExceptionDescriptorEm"))
		{
			uint32_t x = 0;
			if (gCurHle && gCurHle->iProcess)
				for (int m = 0; m < gCurHle->iProcess->iCount && !x; ++m)
					if (aCpu.R[0] >= gCurHle->iProcess->iMod[m].iLoadAddr && aCpu.R[0] < gCurHle->iProcess->iMod[m].iEnd)
						x = gCurHle->iProcess->iMod[m].iExceptionDescriptor;
			aCpu.R[0] = x;
		}
		else if (!strncmp(aName, "_ZN7UserSvr6DllTls", 18))
			aCpu.R[0] = GuestTls(aCpu, aCpu.R[0], false, 0);
		else if (!strncmp(aName, "_ZN7UserSvr9DllSetTls", 21))
		{
			GuestTls(aCpu, aCpu.R[0], true, !strcmp(aName, "_ZN7UserSvr9DllSetTlsEiPv") ? aCpu.R[1] : aCpu.R[2]);
			aCpu.R[0] = 0;
		}
		else
			GuestTls(aCpu, aCpu.R[0], true, 0);
		aCpu.SetReturn();
		return true;
	}
	if (strncmp(aName, "_ZN4User", 8) && strcmp(aName, "_ZNK15XLeaveException9GetReasonEv")) return false;
	if (!strcmp(aName, "_ZN4User16MarkCleanupStackEv"))
	{
		typedef uint32_t (*F)();
		uint32_t h = aNative ? ((F)aNative)() : 0;
		if (!h && !gThr.iTrapLeave && sizeof(void*) == 4)
		{	// the native euser is the setjmp model: its MarkCleanupStack is a stub returning 0 (us_exec.cpp). Mark the level here, as the
			// leave-as-throw euser does: the thread's trap handler (CTrapCleanup) -> Trap() = slot 0 (TTrapHandler: Trap, UnTrap, Leave)
			const uintptr_t th = NativeByName("_ZN4User11TrapHandlerEv");
			h = th ? ((F)th)() : 0;
			if (h) ((void (*)(uint32_t))(uintptr_t)((const uint32_t*)(uintptr_t)*(const uint32_t*)(uintptr_t)h)[0])(h);
		}
		TLeaveState* s = LeaveState(aCpu);
		if (s && s->iMarks < KMaxMarks)
		{
			TMark& m = s->iMark[s->iMarks++];
			for (int i = 0; i < 8; ++i) m.iCtx[i] = aCpu.R[4 + i];
			m.iCtx[8] = aCpu.R[13]; m.iCtx[9] = aCpu.R[14];
			m.iHandler = h; m.iDepth = s->iDepth; m.iResult = 0;
		}
		aCpu.R[0] = h;
	}
	else if (!strcmp(aName, "_ZN4User18UnMarkCleanupStackEP12TTrapHandler"))
	{
		TLeaveState* s = LeaveState(aCpu);
		if (s && s->iMarks) --s->iMarks;
		typedef void (*F)(uint32_t);
		const uint32_t h = aCpu.R[0];
		if (h && !gThr.iTrapLeave && sizeof(void*) == 4)		// (the matching UnTrap = slot 1; the native UnMarkCleanupStack is a stub)
			((F)(uintptr_t)((const uint32_t*)(uintptr_t)*(const uint32_t*)(uintptr_t)h)[1])(h);
		else if (aNative)
			((F)aNative)(h);
	}
	else if (!strcmp(aName, "_ZNK15XLeaveException9GetReasonEv"))
		aCpu.R[0] = aCpu.Read32(aCpu.R[0] + 4);
	else if (!strcmp(aName, "_ZN4User5LeaveEi")) { GuestLeave(aCpu, r0); return true; }
	else if (!strcmp(aName, "_ZN4User12LeaveIfErrorEi")) { if (r0 < 0) { GuestLeave(aCpu, r0); return true; } }
	else if (!strcmp(aName, "_ZN4User13LeaveNoMemoryEv")) { GuestLeave(aCpu, -4); return true; }
	else if (!strcmp(aName, "_ZN4User11LeaveIfNullEPv")) { if (!r0) { GuestLeave(aCpu, -4); return true; } }
	else
		return false;
	aCpu.SetReturn();
	return true;
}

// OUR ARM device euser (protgl-arm, dev_user_execs.cpp) also exports the setjmp TRAP: TTrap::Trap @ 2548, TTrap::UnTrap @ 2549, past the
// frozen 2010 table; ARM DLLs built against it (our ARM builds of the system DLLs) call them. They run on the guest side: Trap marks the
// native cleanup level (the thread's trap handler, slot 0) and keeps the ARM context; a leave restores it (GuestLeave) and Trap returns 1.
enum { KOrdTTrapTrap = 2548, KOrdTTrapUnTrap = 2549 };
static bool TTrapOverride(CArmCpu& aCpu, uint32_t aOrd)
{
	if (aOrd != KOrdTTrapTrap && aOrd != KOrdTTrapUnTrap) return false;
	typedef uint32_t (*F)();
	const uintptr_t th = NativeByName("_ZN4User11TrapHandlerEv");
	TLeaveState* s = LeaveState(aCpu);
	if (aOrd == KOrdTTrapTrap)
	{
		// (a host test's native euser is the Mark model, gThr.iTrapLeave set: the level opens through its MarkCleanupStack instead)
		const uintptr_t mark = gThr.iTrapLeave ? NativeByName("_ZN4User16MarkCleanupStackEv") : 0;
		const uint32_t h = mark ? ((F)mark)() : th ? ((F)th)() : 0;
		if (h && !mark && sizeof(void*) == 4) ((void (*)(uint32_t))(uintptr_t)((const uint32_t*)(uintptr_t)*(const uint32_t*)(uintptr_t)h)[0])(h);
		aCpu.Write32(aCpu.R[1], 0);					// aResult = KErrNone
		if (s && s->iMarks < KMaxMarks)
		{
			TMark& m = s->iMark[s->iMarks++];
			for (int i = 0; i < 8; ++i) m.iCtx[i] = aCpu.R[4 + i];
			m.iCtx[8] = aCpu.R[13]; m.iCtx[9] = aCpu.R[14];
			m.iHandler = h; m.iDepth = s->iDepth; m.iResult = aCpu.R[1];
		}
		aCpu.R[0] = 0;
	}
	else
	{
		uint32_t h = 0;
		if (s && s->iMarks) h = s->iMark[--s->iMarks].iHandler;
		const uintptr_t unmark = gThr.iTrapLeave ? NativeByName("_ZN4User18UnMarkCleanupStackEP12TTrapHandler") : 0;
		if (unmark) ((void (*)(uint32_t))unmark)(h);
		else if (h && sizeof(void*) == 4) ((void (*)(uint32_t))(uintptr_t)((const uint32_t*)(uintptr_t)*(const uint32_t*)(uintptr_t)h)[1])(h);
	}
	aCpu.SetReturn();
	return true;
}

// the guest's catch clause must see drtaeabi's XLeaveException type_info (the one the throw passes), not the native euser's
static bool TypeInfoAlias(int aDll, const char* aName, const char*& aArmDll, uint32_t& aArmOrd);
bool BridgeDataAlias(const char* aDll, uint32_t aOrd, const char*& aArmDll, uint32_t& aArmOrd)
{
	if (gDll[1].iNativeOn && SameDll(aDll, "efsrv.dll") && aOrd >= 1 && aOrd <= gDll[1].iCount)
		return TypeInfoAlias(1, gDll[1].iRows[aOrd - 1].iName, aArmDll, aArmOrd);
	if (!gDll[0].iNativeOn || !SameDll(aDll, "euser.dll") || aOrd < 1 || aOrd > gDll[0].iCount) return false;
	const char* n = gDll[0].iRows[aOrd - 1].iName;
	if (strcmp(n, "_ZTI15XLeaveException") && strcmp(n, "_ZTV15XLeaveException")) return TypeInfoAlias(0, n, aArmDll, aArmOrd);
	if (!strcmp(n, "_ZTI15XLeaveException")) { aArmDll = "drtaeabi.dll"; aArmOrd = KOrdTypeInfo; return true; }
	if (!strcmp(n, "_ZTV15XLeaveException")) { aArmDll = "drtaeabi.dll"; aArmOrd = KOrdVtable; return true; }
	return false;
}

// RTTI (rung R1): the native euser/efsrv are -fno-rtti, the guest's drtaeabi runs dynamic_cast and the catch matching. A guest _ZTI import of a
// bridged DLL resolves to an ARM-layout type_info built here once per process: {drtaeabi's __class_type_info / __si_class_type_info vptr, the
// mangled name, the base's type_info}. One object per class, so the address identity drtaeabi compares holds across all the app's modules.
namespace
{
enum { KOrdClassTiVt = 123, KOrdSiClassTiVt = 127, KMaxTi = 128 };
uint32_t gTiAddr[2][KMaxTi];
const TProcess* gTiProc;
uint32_t ProcExport(CArmCpu& aCpu, const TProcess& aProc, const char* aDll, uint32_t aOrd)
{
	for (int m = 1; m < aProc.iCount; ++m)
		if (SameDll(aProc.iMod[m].iName, aDll) && aOrd >= 1 && aOrd <= aProc.iMod[m].iExportCount)
			return aCpu.Read32(aProc.iMod[m].iExportDir + 4 * (aOrd - 1));
	return 0;
}
uint32_t MakeTi(CArmCpu& aCpu, const TProcess& aProc, int aDll, int aIdx)
{
	if (aIdx < 0 || aIdx >= KMaxTi) return 0;
	if (gTiAddr[aDll][aIdx]) return gTiAddr[aDll][aIdx];
	const TBridgeTi& t = gDll[aDll].iTi[aIdx];
	const uint32_t base = t.iBase >= 0 ? MakeTi(aCpu, aProc, aDll, t.iBase) : 0;
	// the vptr = the vtable export + the offset drtaeabi's own type_info objects use (its XLeaveException type_info is a __class_type_info)
	const uint32_t clsVt = ProcExport(aCpu, aProc, "drtaeabi.dll", KOrdClassTiVt), siVt = ProcExport(aCpu, aProc, "drtaeabi.dll", KOrdSiClassTiVt);
	const uint32_t own = ProcExport(aCpu, aProc, "drtaeabi.dll", KOrdTypeInfo);
	uint32_t adj = own && aCpu.Valid(own, 4) ? aCpu.Read32(own) - clsVt : 8;
	if (adj > 16) adj = 8;
	if (!clsVt) return 0;
	const uint32_t nl = (uint32_t)strlen(t.iMangled) + 1;
	const uint32_t at = gThr.iAlloc ? gThr.iAlloc(12 + nl) : 0;		// guest memory (the embedder's guest allocator)
	if (!at) return 0;
	for (uint32_t k = 0; k < nl; ++k) aCpu.Write8(at + 12 + k, (uint8_t)t.iMangled[k]);
	aCpu.Write32(at, (base && siVt ? siVt : clsVt) + adj);
	aCpu.Write32(at + 4, at + 12);
	aCpu.Write32(at + 8, base);
	gTiAddr[aDll][aIdx] = at;
	if (gTrace && gThr.iLog)
	{
		char line[160];
		snprintf(line, sizeof(line), "[proteus] bridge type_info %s at %x vptr %x (cls %x si %x adj %u) base %x", t.iMangled, at, aCpu.Read32(at), clsVt, siVt, adj, base);
		gThr.iLog(line);
	}
	return at;
}
}

uint32_t BridgeTypeInfo(CArmCpu& aCpu, const TProcess& aProc, uint32_t aKey)
{
	if (gTiProc != &aProc) { memset(gTiAddr, 0, sizeof(gTiAddr)); gTiProc = &aProc; }
	const int dll = (int)(aKey >> 16), idx = (int)(aKey & 0xFFFF);
	return dll < KBridgeDlls && gDll[dll].iTi ? MakeTi(aCpu, aProc, dll, idx) : 0;
}

static bool TypeInfoAlias(int aDll, const char* aName, const char*& aArmDll, uint32_t& aArmOrd)
{
	if (strncmp(aName, "_ZTI", 4) || !gDll[aDll].iTi) return false;
	for (int i = 0; gDll[aDll].iTi[i].iMangled && i < KMaxTi; ++i)
		if (!strcmp(gDll[aDll].iTi[i].iName, aName)) { aArmDll = "*ti"; aArmOrd = (uint32_t)aDll << 16 | (uint32_t)i; return true; }
	return false;
}

bool BridgeTrapSlot(CArmCpu& aCpu, THle& aHle, uint32_t aSlotValue)
{
	const uint32_t dll = (aSlotValue >> 16) & 0x3F;
	if ((aSlotValue >> 22) != KBridgeModule || dll >= (uint32_t)KBridgeDlls) return false;
	return BridgeTrap(aCpu, aHle, gDll[dll].iName, aSlotValue & 0xFFFF);
}

bool BridgeTrap(CArmCpu& aCpu, THle& aHle, const char* aDll, uint32_t aOrd)
{
	gCurHle = &aHle;
	TBridgeDll* d = 0;
	for (int i = 0; i < KBridgeDlls && !d; ++i)
		if (gDll[i].iNativeOn && SameDll(aDll, gDll[i].iName)) d = &gDll[i];
	if (d == &gDll[0] && aOrd > d->iCount)
	{
		RegisterPrimary(aCpu);
		gCurHle = &aHle;
		return TTrapOverride(aCpu, aOrd);
	}
	if (!d || aOrd < 1 || aOrd > d->iCount)
		return false;
	const uint32_t row = aOrd - 1;
	const TBridgeExport& e = d->iRows[row];
	RegisterPrimary(aCpu);
	if (d == &gDll[0] && IsLeaveRow(e.iName) && LeaveOverride(aCpu, e.iName, NativeOf(d, row, aHle)))
		return true;
	if (d == &gDll[0] && IsLibraryRow(e.iName) && (&aCpu == gPrimary || IsLibraryHandleRow(e.iName)) && LibraryOverride(aCpu, e.iName))
		return true;
	if (d == &gDll[0] && (strstr(e.iName, "11RHandleBase") || strstr(e.iName, "8RLibrary")) && aCpu.R[0] && aCpu.Valid(aCpu.R[0], 4) &&
	    (aCpu.Read32(aCpu.R[0]) & 0xFFFF0000u) == KHandleLib0 && gCurHle)
	{	// a guest library pseudo-handle reaching a native RHandleBase/RLibrary row (the kernel would panic KERN-EXEC 0 on it): name it once
		// (only those rows: r0 of any other row need not be an object, and Valid() accepts uncommitted window pages)
		static const char* seen[16]; int k = 0;
		for (; k < 16 && seen[k] && seen[k] != e.iName; ++k) {}
		if (k < 16 && !seen[k])
		{
			seen[k] = e.iName;
			char line[200];
			snprintf(line, sizeof(line), "[proteus] bridge: guest library handle %x passed to native %s (lr %x)", aCpu.Read32(aCpu.R[0]), e.iName, aCpu.R[14]);
			Say(aHle, "%s", line);
		}
	}
	if (d->iKeepHle[row] && (&aCpu == gPrimary || !strcmp(e.iName, "_ZN6RDebug6PrintfEPKcz")))
		return false;		// (a native guest thread's User::Exit etc. go to the native euser: they end that native thread)
	char note[160];
	if (!NativeOf(d, row, aHle))
	{
		{
			snprintf(note, sizeof(note), "bridge: native %s does not export %s", d->iName, e.iName);
			Say(aHle, "[proteus] %s", note);
			strncpy(aCpu.iNote, note, sizeof(aCpu.iNote) - 1);
			aCpu.Halt(EHleStop);
			return true;
		}
	}
	if (!e.iCall)
	{
		snprintf(note, sizeof(note), "bridge: %s needs a hand-written handler (flags %x)", e.iName, (unsigned)e.iFlags);
		Say(aHle, "[proteus] %s", note);
		strncpy(aCpu.iNote, note, sizeof(aCpu.iNote) - 1);
		aCpu.Halt(EHleStop);
		return true;
	}
	if (gWatchOn && aCpu.Read32(gWatch) != gWatchVal)
	{
		char line[200];
		snprintf(line, sizeof(line), "[proteus] watch %x: %x -> %x before %s (lr %x)", gWatch, gWatchVal, aCpu.Read32(gWatch), e.iName, aCpu.R[14]);
		gWatchVal = aCpu.Read32(gWatch);
		Say(aHle, "%s", line);
	}
	const bool traceRow = gTrace || (gTraceDll >> (d - gDll) & 1) || (gTraceHeap && (strstr(e.iName, "Alloc") || strstr(e.iName, "Free") || strstr(e.iName, "Heap")));
	if (traceRow)
	{
		char line[200];
		const uint32_t sp = aCpu.R[13];
		snprintf(line, sizeof(line), "[proteus] bridge %s (%x,%x,%x,%x; %x,%x)", e.iName, aCpu.R[0], aCpu.R[1], aCpu.R[2], aCpu.R[3],
			aCpu.Valid(sp, 8) ? aCpu.Read32(sp) : 0, aCpu.Valid(sp, 8) ? aCpu.Read32(sp + 4) : 0);
		Say(aHle, "%s", line);
	}
	PatchArgVtables(aCpu, *d, row);
	const uint32_t lr = aCpu.R[14];
	const uint32_t argSize = !strncmp(e.iName, "_ZN10RAllocator", 15) ? aCpu.R[1] : aCpu.R[0];	// (the watch: RAllocator::AllocZ(this, size) / User::Alloc(size))
	TGuestHeap heap(aCpu, !strcmp(e.iName, "_ZN4User15SwitchAllocatorEP10RAllocator") || !strcmp(e.iName, "_ZN4User10SwitchHeapEP10RAllocator"));
	if (d->iLeaves && d->iLeaves[row] && gThr.iTrapped)
	{	// a native ...L export: a native leave inside it must not longjmp over this frame -- run it under a native TRAP, raise the leave in the guest
		TTrappedCall t = { &aCpu, e.iCall, d->iFn[row] };
		const int32_t r = gThr.iTrapped(&TTrappedCall::Run, &t);
		if (r != 0)
		{
			GuestLeave(aCpu, r);
			return true;
		}
	}
	else
		e.iCall(aCpu, d->iFn[row]);
	if (gWatchOn && aCpu.Read32(gWatch) != gWatchVal)
	{
		char line[200];
		snprintf(line, sizeof(line), "[proteus] watch %x: %x -> %x in %s (%x,%x,%x)", gWatch, gWatchVal, aCpu.Read32(gWatch), e.iName, aCpu.R[0], aCpu.R[1], aCpu.R[2]);
		gWatchVal = aCpu.Read32(gWatch);
		Say(aHle, "%s", line);
	}
	if (gWatchSize && strstr(e.iName, "Alloc") && (argSize == gWatchSize) && aCpu.R[0])
	{
		gWatch = aCpu.R[0] - 4; gWatchOn = true; gWatchVal = aCpu.Read32(gWatch); gWatchPrev = aCpu.Read32(gWatch - 4);
		char line[160];
		snprintf(line, sizeof(line), "[proteus] watch armed %x (%s %x) [%x %x]", gWatch, e.iName, argSize, gWatchPrev, gWatchVal);
		Say(aHle, "%s", line);
	}
	else if (gWatch && !gWatchOn && aCpu.R[0] == gWatch + 4 && strstr(e.iName, "Alloc")) { gWatchOn = true; gWatchVal = aCpu.Read32(gWatch); gWatchPrev = aCpu.Read32(gWatch - 4); }
	if (traceRow)
	{
		char line[120];
		snprintf(line, sizeof(line), "[proteus] bridge = %x (heap %x)", aCpu.R[0], aCpu.iGuestAlloc);
		Say(aHle, "%s", line);
	}
	aCpu.R[14] = aCpu.iJmpPc ? aCpu.iJmpPc : lr;
	aCpu.iJmpPc = 0;
	aCpu.SetReturn();
	return true;
}
}

namespace proteus
{
// the name a native ordinal of aDll is bound under (the host tests' fake native tables key on names)
const char* BridgeNativeName(const char* aDll, uint32_t aNativeOrdinal)
{
	const uint32_t KCounts[KBridgeDlls] = { KBridge_euser_Count, KBridge_efsrv_Count, KBridge_libc_Count, KBridge_libpthread_Count };
	for (int i = 0; i < KBridgeDlls; ++i)
		if (SameDll(aDll, gDll[i].iName))
			for (uint32_t r = 0; r < KCounts[i]; ++r)
				if (gDll[i].iRows[r].iNative == aNativeOrdinal) return gDll[i].iRows[r].iName;
	return "";
}
}

// ---- the varargs of descriptor formatting (R1 gate 2). An ARM caller passes `...` in the AAPCS layout (r2.. then the stack; a 64-bit value or a
// double in an even register pair / 8-aligned slot; a float is promoted to double); the native TDes::FormatList / AppendFormatList want an x32
// SysV va_list. The format string says what each argument is, so the ARM layout is read argument by argument into 8-byte slots (the overflow
// area of a va_list whose register areas are marked used) and the native function is called with that va_list.
namespace proteus
{
namespace
{
struct TVaTag { unsigned iGp, iFp; void* iOverflow; void* iRegSave; };	// the SysV x86-64 / x32 __va_list_tag
// where the next ARM variadic argument comes from: the registers + stack of the call, or (an ARM va_list) guest memory at a pointer
struct TArmVa
{
	BrArgs* iArgs;
	CArmCpu& iCpu;
	uint32_t iAp;
	bool iMem;
	uint32_t W() { if (!iMem) return iArgs->W(); const uint32_t v = iCpu.Read32(iAp); iAp += 4; return v; }
	uint64_t L() { if (!iMem) return iArgs->L(); iAp = (iAp + 7) & ~7u; const uint64_t v = iCpu.Read32(iAp) | (uint64_t)iCpu.Read32(iAp + 4) << 32; iAp += 8; return v; }
};
enum { KMaxVaSlots = 64, KMaxFmt = 512 };

// the characters of a descriptor (guest address aDesc) as bytes: narrow, or the low byte of a 16-bit character; the type is in the top nibble
// of iLength (EBufC 0 / EPtrC 1 / EPtr 2 / EBuf 3)
int DescBytes(CArmCpu& aCpu, uint32_t aDesc, bool aWide, char* aOut, int aMax)
{
	const uint32_t w = aCpu.Read32(aDesc);
	uint32_t len = w & 0x0FFFFFFF, p;
	switch (w >> 28)
	{
	case 1: p = aCpu.Read32(aDesc + 4); break;
	case 2: p = aCpu.Read32(aDesc + 8); break;
	case 3: p = aDesc + 8; break;
	default: p = aDesc + 4; break;
	}
	if ((int)len > aMax - 1) len = (uint32_t)aMax - 1;
	for (uint32_t i = 0; i < len; ++i) aOut[i] = (char)(aWide ? aCpu.Read16(p + 2 * i) : aCpu.Read8(p + i));
	aOut[len] = 0;
	return (int)len;
}

// the format's conversions -> the slots (Symbian: %d %i %u %x %X %o %c = a word, %Ld %Lu %Lx = 64 bits, %e %f %g = a double, %s %S %p = a pointer,
// '*' = a word for the width / precision, %% = none)
int BuildSlots(const char* aFmt, TArmVa& aVa, uint64_t* aSlots)
{
	int n = 0;
	for (const char* s = aFmt; *s && n < KMaxVaSlots - 4; ++s)
	{
		if (*s != '%') continue;
		++s;
		while (*s == '-' || *s == '+' || *s == '0' || *s == ' ' || *s == '#') ++s;
		if (*s == '*') { aSlots[n++] = aVa.W(); ++s; } else while (*s >= '0' && *s <= '9') ++s;
		if (*s == '.')
		{
			++s;
			if (*s == '*') { aSlots[n++] = aVa.W(); ++s; } else while (*s >= '0' && *s <= '9') ++s;
		}
		bool big = false;
		while (*s == 'l' || *s == 'L' || *s == 'h') { if (*s == 'L') big = true; ++s; }
		switch (*s)
		{
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
			if (big) aSlots[n++] = aVa.L(); else aSlots[n++] = aVa.W();
			break;
		case 'e': case 'E': case 'f': case 'g': case 'G':
			aSlots[n++] = aVa.L();
			break;
		case 's': case 'S': case 'p':
			aSlots[n++] = aVa.W();
			break;
		case 0: return n;
		default: break;			// %% and unknown conversions take nothing
		}
	}
	return n;
}

// kinds: 0 Format(this, fmt, ...)  1 AppendFormat(this, fmt, ...)  2 AppendFormat(this, fmt, overflow, ...)  3 FormatList(this, fmt, va)  4 AppendFormatList(this, fmt, va, overflow)
void FormatCall(CArmCpu& aCpu, bool aWide, int aKind)
{
	BrArgs a(aCpu);
	const uint32_t self = a.W(), fmtDesc = a.W();
	uint32_t overflow = 0, vaPtr = 0;
	if (aKind == 2) overflow = a.W();
	if (aKind == 3 || aKind == 4) vaPtr = a.W();
	if (aKind == 4) overflow = a.W();
	char fmt[KMaxFmt];
	DescBytes(aCpu, fmtDesc, aWide, fmt, sizeof(fmt));
	TArmVa va = { &a, aCpu, vaPtr, aKind >= 3 };
	uint64_t slots[KMaxVaSlots];
	BuildSlots(fmt, va, slots);
	TVaTag tag = { 48, 176, slots, 0 };
	static const char* const KList[2] = { "_ZN5TDes810FormatListERK6TDesC8St9__va_list", "_ZN6TDes1610FormatListERK7TDesC16St9__va_list" };
	static const char* const KAppend[2] = { "_ZN5TDes816AppendFormatListERK6TDesC8St9__va_listP13TDes8Overflow", "_ZN6TDes1616AppendFormatListERK7TDesC16St9__va_listP14TDes16Overflow" };
	const bool replace = aKind == 0 || aKind == 3;
	const uintptr_t fn = NativeByName((replace ? KList : KAppend)[aWide]);
	if (!fn)
	{
		strncpy(aCpu.iNote, "bridge: native TDes::(Append)FormatList is not exported", sizeof(aCpu.iNote) - 1);
		aCpu.Halt(EHleStop);
		return;
	}
	typedef void (*F)(uint32_t, uint32_t, void*, uint32_t);
	((F)fn)(self, fmtDesc, &tag, overflow);
}
}

void BrOvr_Format16(CArmCpu& c, uintptr_t) { FormatCall(c, true, 0); }
void BrOvr_AppendFormat16(CArmCpu& c, uintptr_t) { FormatCall(c, true, 1); }
void BrOvr_AppendFormatOvf16(CArmCpu& c, uintptr_t) { FormatCall(c, true, 2); }
void BrOvr_FormatList16(CArmCpu& c, uintptr_t) { FormatCall(c, true, 3); }
void BrOvr_AppendFormatList16(CArmCpu& c, uintptr_t) { FormatCall(c, true, 4); }
void BrOvr_Format8(CArmCpu& c, uintptr_t) { FormatCall(c, false, 0); }
void BrOvr_AppendFormat8(CArmCpu& c, uintptr_t) { FormatCall(c, false, 1); }
void BrOvr_AppendFormatOvf8(CArmCpu& c, uintptr_t) { FormatCall(c, false, 2); }
void BrOvr_FormatList8(CArmCpu& c, uintptr_t) { FormatCall(c, false, 3); }
void BrOvr_AppendFormatList8(CArmCpu& c, uintptr_t) { FormatCall(c, false, 4); }

// ---- Open C's libc natively (rung R1 round 16): its varargs and the calls that must stay in the guest. The printf / scanf families read the ARM
// `...` (or an ARM va_list = a pointer into the guest stack) conversion by conversion into 8-byte slots and call the native v-function with a
// SysV va_list over them (as FormatCall above). setjmp / longjmp save and restore the GUEST's registers (a native setjmp would save the
// handler's); exit flushes the native stdio and ends the guest the way User::Exit does.
namespace
{
uintptr_t NativeLibc(const char* aName)
{
	TBridgeDll* d = &gDll[2];
	for (uint32_t r = 0; r < d->iCount; ++r)
		if (!strcmp(d->iRows[r].iName, aName)) return gCurHle ? NativeOf(d, r, *gCurHle) : 0;
	return 0;
}

int GuestFmt(CArmCpu& aCpu, uint32_t aFmt, bool aWide, char* aOut, int aMax)
{
	int n = 0;
	for (; n < aMax - 1; ++n)
	{
		const uint32_t c = aWide ? aCpu.Read16(aFmt + 2 * n) : aCpu.Read8(aFmt + n);
		if (!c) break;
		aOut[n] = c < 0x80 ? (char)c : '?';	// the walker only needs the ASCII conversion letters
	}
	aOut[n] = 0;
	return n;
}

// C conversions -> slots. printf: hh h l z t j = a word on ARM (long and size_t are 32 bits), ll q = 64 bits, a e f g (L too: long double
// = double on ARM) = a double, c s p n C S = a word. scanf: every conversion that is not %* or %% is one pointer.
int BuildSlotsC(const char* aFmt, TArmVa& aVa, uint64_t* aSlots, bool aScan)
{
	int n = 0;
	for (const char* s = aFmt; *s && n < KMaxVaSlots - 4; ++s)
	{
		if (*s != '%') continue;
		++s;
		if (*s == '%') continue;
		if (aScan)
		{
			const bool skip = *s == '*';
			while (*s && !strchr("diouxXaAeEfFgGcsSCpn[", *s)) ++s;
			if (*s == '[') { ++s; if (*s == '^') ++s; if (*s == ']') ++s; while (*s && *s != ']') ++s; }
			if (!*s) break;
			if (!skip) aSlots[n++] = aVa.W();
			continue;
		}
		while (*s && strchr("-+ 0#'", *s)) ++s;
		if (*s == '*') { aSlots[n++] = aVa.W(); ++s; } else while (*s >= '0' && *s <= '9') ++s;
		if (*s == '.')
		{
			++s;
			if (*s == '*') { aSlots[n++] = aVa.W(); ++s; } else while (*s >= '0' && *s <= '9') ++s;
		}
		int longs = 0;
		while (*s && strchr("hlLqjzt", *s)) { if (*s == 'l') ++longs; if (*s == 'q' || *s == 'j') longs = 2; ++s; }
		if (!*s) break;
		switch (*s)
		{
		case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
			if (longs >= 2) aSlots[n++] = aVa.L(); else aSlots[n++] = aVa.W();
			break;
		case 'a': case 'A': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
			aSlots[n++] = aVa.L();
			break;
		default:
			aSlots[n++] = aVa.W();
			break;
		}
	}
	return n;
}

// aFixed words before the format, then the format; aList = an ARM va_list follows it (the v-functions), else `...`
void LibcFormatCall(CArmCpu& aCpu, const char* aNativeV, int aFixed, bool aWide, bool aScan, bool aList)
{
	BrArgs a(aCpu);
	uint32_t w[3] = { 0, 0, 0 };
	for (int i = 0; i < aFixed; ++i) w[i] = a.W();
	const uint32_t fmt = a.W();
	const uint32_t ap = aList ? a.W() : 0;
	char f[KMaxFmt];
	GuestFmt(aCpu, fmt, aWide, f, sizeof(f));
	TArmVa va = { &a, aCpu, ap, aList };
	uint64_t slots[KMaxVaSlots];
	BuildSlotsC(f, va, slots, aScan);
	TVaTag tag = { 48, 176, slots, 0 };
	const uintptr_t fn = NativeLibc(aNativeV);
	if (!fn)
	{
		snprintf(aCpu.iNote, sizeof(aCpu.iNote), "bridge: native libc does not export %s", aNativeV);
		aCpu.Halt(EHleStop);
		return;
	}
	typedef int (*F0)(uint32_t, void*);
	typedef int (*F1)(uint32_t, uint32_t, void*);
	typedef int (*F2)(uint32_t, uint32_t, uint32_t, void*);
	int r;
	if (aFixed == 0) r = ((F0)fn)(fmt, &tag);
	else if (aFixed == 1) r = ((F1)fn)(w[0], fmt, &tag);
	else r = ((F2)fn)(w[0], w[1], fmt, &tag);
	aCpu.R[0] = (uint32_t)r;
}
}

#define BR_LIBC_FMT(name, v, fixed, wide, scan, list) void BrOvr_libc_##name(CArmCpu& c, uintptr_t) { LibcFormatCall(c, #v, fixed, wide, scan, list); }
BR_LIBC_FMT(printf, vprintf, 0, false, false, false)
BR_LIBC_FMT(fprintf, vfprintf, 1, false, false, false)
BR_LIBC_FMT(sprintf, vsprintf, 1, false, false, false)
BR_LIBC_FMT(snprintf, vsnprintf, 2, false, false, false)
BR_LIBC_FMT(asprintf, vasprintf, 1, false, false, false)
BR_LIBC_FMT(scanf, vscanf, 0, false, true, false)
BR_LIBC_FMT(fscanf, vfscanf, 1, false, true, false)
BR_LIBC_FMT(sscanf, vsscanf, 1, false, true, false)
BR_LIBC_FMT(wprintf, vwprintf, 0, true, false, false)
BR_LIBC_FMT(fwprintf, vfwprintf, 1, true, false, false)
BR_LIBC_FMT(swprintf, vswprintf, 2, true, false, false)
BR_LIBC_FMT(wscanf, vwscanf, 0, true, true, false)
BR_LIBC_FMT(fwscanf, vfwscanf, 1, true, true, false)
BR_LIBC_FMT(swscanf, vswscanf, 1, true, true, false)
BR_LIBC_FMT(vprintf, vprintf, 0, false, false, true)
BR_LIBC_FMT(vfprintf, vfprintf, 1, false, false, true)
BR_LIBC_FMT(vsprintf, vsprintf, 1, false, false, true)
BR_LIBC_FMT(vsnprintf, vsnprintf, 2, false, false, true)
BR_LIBC_FMT(vasprintf, vasprintf, 1, false, false, true)
BR_LIBC_FMT(vscanf, vscanf, 0, false, true, true)
BR_LIBC_FMT(vfscanf, vfscanf, 1, false, true, true)
BR_LIBC_FMT(__vfscanf, __vfscanf, 1, false, true, true)
BR_LIBC_FMT(vsscanf, vsscanf, 1, false, true, true)
BR_LIBC_FMT(vwprintf, vwprintf, 0, true, false, true)
BR_LIBC_FMT(vfwprintf, vfwprintf, 1, true, false, true)
BR_LIBC_FMT(vswprintf, vswprintf, 2, true, false, true)
BR_LIBC_FMT(vwscanf, vwscanf, 0, true, true, true)
BR_LIBC_FMT(vfwscanf, vfwscanf, 1, true, true, true)
BR_LIBC_FMT(vswscanf, vswscanf, 1, true, true, true)
#undef BR_LIBC_FMT

// open(path, flags, ...) / fcntl(fd, cmd, ...) / ioctl(fd, req, ...) / semctl(id, num, cmd, ...): the optional argument is one word (a mode,
// an int, a pointer, union semun); the native function is called with it always
void BrOvr_libc_Words3(CArmCpu& c, uintptr_t aFn)
{
	typedef int (*F)(uint32_t, uint32_t, uint32_t);
	c.R[0] = (uint32_t)((F)aFn)(c.R[0], c.R[1], c.R[2]);
}
void BrOvr_libc_Words4(CArmCpu& c, uintptr_t aFn)
{
	typedef int (*F)(uint32_t, uint32_t, uint32_t, uint32_t);
	c.R[0] = (uint32_t)((F)aFn)(c.R[0], c.R[1], c.R[2], c.R[3]);
}

// the guest's process ends (as User::Exit, KKeepHle): exit flushes the native stdio first; a native guest thread ends natively
void BrOvr_libc_exit(CArmCpu& c, uintptr_t aFn)
{
	if (!BridgeIsGuestThread(c))
	{
		typedef int (*Fl)(uint32_t);
		const uintptr_t fl = NativeLibc("fflush");
		if (fl) ((Fl)fl)(0);
		c.Halt(EExited, (int32_t)c.R[0]);
		return;
	}
	typedef void (*F)(uint32_t);
	((F)aFn)(c.R[0]);
}
void BrOvr_libc__exit(CArmCpu& c, uintptr_t aFn)
{
	if (!BridgeIsGuestThread(c)) { c.Halt(EExited, (int32_t)c.R[0]); return; }
	typedef void (*F)(uint32_t);
	((F)aFn)(c.R[0]);
}
void BrOvr_libc_abort(CArmCpu& c, uintptr_t aFn)
{
	typedef void (*F)();
	((F)aFn)();
}

// setjmp(buf): r4-r11, sp, lr of the guest (10 of the ARM jmp_buf's 16 words); longjmp(buf, v): back to that lr with v (0 -> 1)
void BrOvr_libc_setjmp(CArmCpu& c, uintptr_t)
{
	const uint32_t b = c.R[0];
	for (int i = 0; i < 8; ++i) c.Write32(b + 4 * i, c.R[4 + i]);
	c.Write32(b + 32, c.R[13]);
	c.Write32(b + 36, c.R[14]);
	c.R[0] = 0;
}
void BrOvr_libc_longjmp(CArmCpu& c, uintptr_t)
{
	const uint32_t b = c.R[0], v = c.R[1];
	for (int i = 0; i < 8; ++i) c.R[4 + i] = c.Read32(b + 4 * i);
	c.R[13] = c.Read32(b + 32);
	c.iJmpPc = c.Read32(b + 36);
	c.R[0] = v ? v : 1;
}

// ---- the process asking about itself (mixed mode). The native kernel's answers are the HANDLER's: RProcess::FileName is
// z:\sys\bin\cpuhandler_arm.exe, Type its UIDs, User::CommandLine starts with the ARM program's path (the loader puts it in front).
// apparc names the app's resource file after FileName (CEikApplication::ResourceFileName): with the handler's name no resource file is
// found and the first resource read lands in another file (PyS60: USER 23 in TDes8::Copy under bafl). The guest's own answers, as the
// HLE executive gives them in pure ARM mode (proteusexec.cpp EExecProcessFileName / EExecProcessType); another process's handle stays native.
namespace
{
const uint32_t KCurrentProcess = 0xFFFF8000u;
void WriteBuf16(CArmCpu& aCpu, uint32_t aDes, uint32_t aMax, const char* aText)
{
	uint32_t n = 0;
	for (; aText[n] && n < aMax; ++n) aCpu.Write16(aDes + 8 + 2 * n, (uint8_t)aText[n]);
	aCpu.Write32(aDes, (3u << 28) | n);		// EBuf
	aCpu.Write32(aDes + 4, aMax);
}
uint32_t ExePrefix()		// the length of "<exe path> " the native command line starts with (0 = none)
{
	return gCurHle && gCurHle->iExePath ? (uint32_t)strlen(gCurHle->iExePath) + 1 : 0;
}
}

void BrOvr_ProcessFileName(CArmCpu& aCpu, uintptr_t aFn)		// TFileName RProcess::FileName() const: r0 = result, r1 = this
{
	const uint32_t ret = aCpu.R[0], self = aCpu.R[1];
	if (gCurHle && gCurHle->iExePath && aCpu.Read32(self) == KCurrentProcess)
		WriteBuf16(aCpu, ret, 256, gCurHle->iExePath);
	else
	{
		typedef void (*F)(void*, uint32_t);
		((F)aFn)(aCpu.HostPtr(ret), self);
	}
}

void BrOvr_ProcessType(CArmCpu& aCpu, uintptr_t aFn)		// TUidType RProcess::Type() const: r0 = result, r1 = this
{
	const uint32_t ret = aCpu.R[0], self = aCpu.R[1];
	const TLoaded* m = gCurHle ? gCurHle->iImage : 0;
	if (m && aCpu.Read32(self) == KCurrentProcess)
	{
		aCpu.Write32(ret, m->iUid1); aCpu.Write32(ret + 4, 0x100039ce); aCpu.Write32(ret + 8, m->iUid3);
		return;
	}
	struct TUid3 { uint32_t iU[3]; };
	typedef TUid3 (*F)(uint32_t);
	const TUid3 t = ((F)aFn)(self);
	for (int i = 0; i < 3; ++i) aCpu.Write32(ret + 4 * i, t.iU[i]);
}

void BrOvr_CommandLineLength(CArmCpu& aCpu, uintptr_t aFn)		// TInt User::CommandLineLength(): without the program path in front
{
	typedef uint32_t (*F)();
	const uint32_t n = ((F)aFn)(), p = ExePrefix();
	aCpu.R[0] = p && n >= p ? n - p : (p && n + 1 == p ? 0 : n);
}

void BrOvr_CommandLine(CArmCpu& aCpu, uintptr_t aFn)		// void User::CommandLine(TDes16&): the native line, the program path removed
{
	// the kernel writes the WHOLE native line (path in front) and panics KERN-EXEC 35 (EKUDesSetLengthOverflow) when the caller sized its
	// descriptor by our CommandLineLength (the line without the path): read it into a TBuf16 of the native length below the guest's sp
	// (or from the guest allocator when long), then copy the rest of the line into the caller's descriptor
	const uint32_t des = aCpu.R[0];
	typedef void (*F)(uint32_t);
	typedef uint32_t (*FL)();
	const uint32_t p = ExePrefix();
	uintptr_t lenFn = p && gCurHle->iIpc && gCurHle->iIpc->iNativeExport ? gCurHle->iIpc->iNativeExport(gCurHle->iIpc->iCtx, "euser.dll", 622) : 0;
	if (lenFn && gResolve) lenFn = gResolve((uint32_t)lenFn);		// (the host test hands out indices)
	if (!p || !des || !lenFn)		// (622 = User::CommandLineLength)
	{
		((F)aFn)(des);
		return;
	}
	const uint32_t n = ((FL)lenFn)(), bytes = (8 + 2 * n + 7) & ~7u;
	uint32_t tmp = (aCpu.R[13] - 128 - bytes) & ~7u;		// below the frame a guest call would start at (sp - 64)
	if (bytes > 4096 || !aCpu.Valid(tmp, bytes)) tmp = gThr.iAlloc ? gThr.iAlloc(bytes) : 0;		// (a long line: guest heap, not freed)
	if (!tmp) return;
	aCpu.Write32(tmp, 3u << 28); aCpu.Write32(tmp + 4, n);		// an empty TBuf16<n>
	((F)aFn)(tmp);
	const uint32_t len = aCpu.Read32(tmp) & 0x0FFFFFFF;
	uint32_t skip = 0;
	if (len + 1 >= p)		// only when the line really starts with the path
	{
		skip = len >= p ? p : len;
		for (uint32_t i = 0; i + 1 < p; ++i)
		{
			uint32_t c = aCpu.Read16(tmp + 8 + 2 * i), e = (uint8_t)gCurHle->iExePath[i];
			if (c >= 'A' && c <= 'Z') c += 32;
			if (e >= 'A' && e <= 'Z') e += 32;
			if (c != e) { skip = 0; break; }
		}
	}
	const uint32_t w0 = aCpu.Read32(des), type = w0 >> 28;
	const uint32_t max = type == 1 || type == 0 ? 0 : aCpu.Read32(des + 4);		// TDes16: iMaxLength
	const uint32_t buf = type == 2 ? aCpu.Read32(des + 8) : type == 3 ? des + 8 : type == 4 ? aCpu.Read32(des + 8) + 4 : 0;	// TPtr / TBuf / RBuf(HBufC)
	const uint32_t rest = len - skip, m = rest < max ? rest : max;		// (the native call would have panicked on overflow; truncate instead)
	if (!buf) return;
	for (uint32_t i = 0; i < m; ++i) aCpu.Write16(buf + 2 * i, (uint16_t)aCpu.Read16(tmp + 8 + 2 * (skip + i)));
	aCpu.Write32(des, (w0 & 0xF0000000u) | m);
	if (type == 4) aCpu.Write32(aCpu.Read32(des + 8), m);		// an RBuf over an HBufC: the cell's length too
}

void BrOvr_ExeExportData(CArmCpu& aCpu, uintptr_t)		// TAny* RProcess::ExeExportData(): the EXE's named-symbol export data, ordinal 0
{
	// the kernel answers from the process' code segment (DCodeSeg::Lookup(0)) = the handler's image; the program's is the guest EXE's:
	// the word before its export directory when the header declares KImageNmdExpData (f32image.h 0x400), else NULL (not a STDEXE)
	const TLoaded* m = gCurHle ? gCurHle->iImage : 0;
	aCpu.R[0] = m && (m->iFlags & 0x400u) && m->iExportDir ? aCpu.Read32(m->iExportDir - 4) : 0;
}

}

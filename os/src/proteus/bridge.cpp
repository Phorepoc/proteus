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

uint32_t BridgeCallGuest(CArmCpu& aCpu, uint32_t aFn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	CArmCpu& c = aCpu;
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
TBridgeThreads gThr;
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
	const uint32_t stack = gThr.iAlloc(KGuestStack);
	if (!stack) return 0;
	CArmCpu* c = new CArmCpu(*gPrimary);				// the memory view (region, page map, windows) and the hooks are shared
	memset(c->R, 0, sizeof(c->R)); memset(c->iD, 0, sizeof(c->iD));
	c->iN = c->iZ = c->iC = c->iV = c->iT = c->iGE = 0;
	c->iFpscr = 0; c->iTpidrurw = c->iTpidruro = 0;
	c->iHalt = ERunning; c->iExitCode = 0; c->iSteps = 0; c->iExcl = false;
	c->iSliceSteps = 0; c->iNextSlice = 0;				// no guest scheduler: the kernel schedules the thread
	c->iStatSlow = c->iStatSvc = c->iStatTrap = c->iStatMem = 0;
	c->iNote[0] = 0;
	c->iJit = gThr.iJitNew ? gThr.iJitNew(*c) : 0;
	if (!SetCpuOfThisThread(c, 0)) { if (c->iJit && gThr.iJitDelete) gThr.iJitDelete(c->iJit); delete c; if (gThr.iFree) gThr.iFree(stack); return 0; }
	c->R[0] = a0; c->R[1] = a1; c->R[2] = a2; c->R[3] = a3;
	c->R[13] = (stack + KGuestStack) & ~7u;
	c->R[14] = KReturnToHost;
	c->Branch(aFn);
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
	delete c;
	if (gThr.iFree) gThr.iFree(stack);
	if (left)
		NativeLeave(leave);					// a guest thread function that left with no TRAP: the native thread leaves (and panics, as natively)
	return result;
}

namespace
{
enum { KMaxStubs = 512 };
struct TRevStub { uint32_t iFn; };
TRevStub gStub[KMaxStubs];
int gStubCount;
template <int K> uint32_t RevStub(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	CArmCpu* cpu = CpuOfThisThread();
	return cpu ? BridgeCallGuest(*cpu, gStub[K].iFn, a, b, c, d) : RunOnNewCpu(gStub[K].iFn, a, b, c, d);
}
typedef uint32_t (*TStubFn)(uint32_t, uint32_t, uint32_t, uint32_t);
template <int K> struct TStubTable { static void Fill(TStubFn* t) { t[K] = &RevStub<K>; TStubTable<K - 1>::Fill(t); } };
template <> struct TStubTable<-1> { static void Fill(TStubFn*) {} };
TStubFn gStubFn[KMaxStubs];
}

// the native address of stub k: on x32 the function itself; on an LP64 host test a token BridgeStubFn turns back into the function
static uint32_t StubAddress(int k) { return sizeof(void*) == 4 ? (uint32_t)(uintptr_t)gStubFn[k] : 0xE0000000u | (uint32_t)k; }
uintptr_t BridgeStubFn(uint32_t aAddr) { return (aAddr & 0xFF000000u) == 0xE0000000u ? (uintptr_t)gStubFn[aAddr & 0xFFFFFF] : (uintptr_t)aAddr; }

// the guest function behind a reverse stub address (0 = not a stub): guest code reached a stub through a patched vtable slot (see PatchVtable)
uint32_t BridgeStubTarget(uint32_t aPc)
{
	const int n = __atomic_load_n(&gStubCount, __ATOMIC_ACQUIRE);
	for (int k = 0; k < n; ++k)
		if (StubAddress(k) == (aPc & ~1u)) return gStub[k].iFn;
	return 0;
}

static uint32_t BrCb(CArmCpu& aCpu, uint32_t aGuestFn)
{
	if (!aGuestFn || !aCpu.InRegion(aGuestFn & ~1u, 2))
		return aGuestFn;
	if (!gStubFn[0]) TStubTable<KMaxStubs - 1>::Fill(gStubFn);
	TLock lock;
	for (int k = 0; k < gStubCount; ++k)
		if (gStub[k].iFn == aGuestFn) return StubAddress(k);
	if (gStubCount == KMaxStubs) { aCpu.Halt(EHleStop); strncpy(aCpu.iNote, "bridge: out of callback stubs", sizeof(aCpu.iNote) - 1); return 0; }
	gStub[gStubCount].iFn = aGuestFn;
	__atomic_store_n(&gStubCount, gStubCount + 1, __ATOMIC_RELEASE);
	return StubAddress(gStubCount - 1);
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
	int16_t* iVtRow;		// per row: its first entry there, -1 = none
};
// imports that are not bridged even for a native DLL: the guest EXE's start-up and exit belong to Proteus, not to the native process
const char* const KKeepHle[] = { "_ZN4User11InitProcessEv", "_ZN7UserSvr9DllSetTlsEiiPv", "_ZN4User4ExitEi", "_ZN6RDebug6PrintfEPKcz" };
TBridgeDll gDll[2] = { { "euser.dll", KBridge_euser, 0, false, 0, 0, KBridgeVt_euser, 0 }, { "efsrv.dll", KBridge_efsrv, 0, false, 0, 0, KBridgeVt_efsrv, 0 } };
bool gTrace;
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

void BridgeConfigure(const char* aText)
{
	gDll[0].iCount = KBridge_euser_Count;
	gDll[1].iCount = KBridge_efsrv_Count;
	for (int i = 0; i < 2; ++i)
		gDll[i].iNativeOn = false;
	gTrace = false;
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
			for (int i = 0; i < 2; ++i) { if (SameDll(a, gDll[i].iName)) gDll[i].iNativeOn = true; }
		else if (!strcmp(w, "trace"))
			gTrace = true;
		else if (!strcmp(w, "map") && sscanf(line, "%*s %lx %lx", &x, &y) == 2)
			{ gLo = (uint32_t)x; gHi = (uint32_t)y; gMap = true; }
		else if (!strcmp(w, "map"))
			gMap = true;
	}
	for (int i = 0; i < 2; ++i)
	{
		TBridgeDll& d = gDll[i];
		free(d.iFn); free(d.iKeepHle);
		d.iFn = (uintptr_t*)calloc(d.iCount, sizeof(uintptr_t));
		d.iKeepHle = (bool*)calloc(d.iCount, sizeof(bool));
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
	for (int i = 0; i < 2; ++i)
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
	for (int i = 0; i < 2; ++i)
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
static void PatchVtable(CArmCpu& aCpu, uint32_t aObj, int aSlots)
{
	if (!aObj || !aCpu.Valid(aObj, 4)) return;			// (the object may be on the native heap: a window, not the region)
	const uint32_t vt = aCpu.Read32(aObj);
	if (!aCpu.InRegion(vt, 4u * aSlots)) return;			// a native vtable (a native-created object): nothing to do
	for (int k = 0; k < aSlots; ++k)
	{
		const uint32_t f = aCpu.Read32(vt + 4 * k);
		if (f && aCpu.InRegion(f & ~1u, 2))
			aCpu.Write32(vt + 4 * k, BrCb(aCpu, f));
	}
}

static void PatchArgVtables(CArmCpu& aCpu, const TBridgeDll& aDll, uint32_t aRow)
{
	const int16_t k0 = aDll.iVtRow ? aDll.iVtRow[aRow] : -1;
	if (k0 < 0) return;
	BrArgs a(aCpu);
	for (int k = k0; aDll.iVt[k].iName && !strcmp(aDll.iVt[k].iName, aDll.iRows[aRow].iName); ++k)
		PatchVtable(aCpu, a.Raw(aDll.iVt[k].iWord), aDll.iVt[k].iSlots);
}

uint32_t BridgeNativeCall(CArmCpu& aCpu, uint32_t aPc)
{
	if (!gDll[0].iNativeOn) return 0;
	const uintptr_t fn = gResolve ? gResolve(aPc) : (uintptr_t)aPc;
	if (!fn) return 0;
	if (gTrace && gCurHle)
	{
		char line[120];
		snprintf(line, sizeof(line), "[proteus] bridge native-call %x (%x,%x,%x,%x)", aPc, aCpu.R[0], aCpu.R[1], aCpu.R[2], aCpu.R[3]);
		Say(*gCurHle, "%s", line);
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
struct TMark { uint32_t iCtx[10]; uint32_t iHandler; int iDepth; };
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
		if (m.iHandler && sizeof(void*) == 4)
		{	// TTrapHandler: Trap, UnTrap, Leave -- slot 2 (no virtual destructor)
			const uint32_t vt = *(const uint32_t*)(uintptr_t)m.iHandler;
			typedef void (*TLeaveFn)(uint32_t, int32_t);
			((TLeaveFn)(uintptr_t)((const uint32_t*)(uintptr_t)vt)[2])(m.iHandler, aReason);
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

static bool IsLeaveRow(const char* n)
{
	static const char* const K[] = { "_ZN4User16MarkCleanupStackEv", "_ZN4User18UnMarkCleanupStackEP12TTrapHandler", "_ZNK15XLeaveException9GetReasonEv",
		"_ZN4User5LeaveEi", "_ZN4User12LeaveIfErrorEi", "_ZN4User13LeaveNoMemoryEv", "_ZN4User11LeaveIfNullEPv" };
	for (const char* k : K) if (!strcmp(n, k)) return true;
	return false;
}
// the hand-written euser rows of the leave model; true = handled (the guest continues at lr, or was redirected)
static bool LeaveOverride(CArmCpu& aCpu, const char* aName, uintptr_t aNative)
{
	if (strncmp(aName, "_ZN4User", 8) && strcmp(aName, "_ZNK15XLeaveException9GetReasonEv")) return false;
	const int32_t r0 = (int32_t)aCpu.R[0];
	if (!strcmp(aName, "_ZN4User16MarkCleanupStackEv"))
	{
		typedef uint32_t (*F)();
		const uint32_t h = aNative ? ((F)aNative)() : 0;
		TLeaveState* s = LeaveState(aCpu);
		if (s && s->iMarks < KMaxMarks)
		{
			TMark& m = s->iMark[s->iMarks++];
			for (int i = 0; i < 8; ++i) m.iCtx[i] = aCpu.R[4 + i];
			m.iCtx[8] = aCpu.R[13]; m.iCtx[9] = aCpu.R[14];
			m.iHandler = h; m.iDepth = s->iDepth;
		}
		aCpu.R[0] = h;
	}
	else if (!strcmp(aName, "_ZN4User18UnMarkCleanupStackEP12TTrapHandler"))
	{
		TLeaveState* s = LeaveState(aCpu);
		if (s && s->iMarks) --s->iMarks;
		typedef void (*F)(uint32_t);
		if (aNative) ((F)aNative)(aCpu.R[0]);
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

// the guest's catch clause must see drtaeabi's XLeaveException type_info (the one the throw passes), not the native euser's
bool BridgeDataAlias(const char* aDll, uint32_t aOrd, const char*& aArmDll, uint32_t& aArmOrd)
{
	if (!gDll[0].iNativeOn || !SameDll(aDll, "euser.dll") || aOrd < 1 || aOrd > gDll[0].iCount) return false;
	const char* n = gDll[0].iRows[aOrd - 1].iName;
	if (!strcmp(n, "_ZTI15XLeaveException")) { aArmDll = "drtaeabi.dll"; aArmOrd = KOrdTypeInfo; return true; }
	if (!strcmp(n, "_ZTV15XLeaveException")) { aArmDll = "drtaeabi.dll"; aArmOrd = KOrdVtable; return true; }
	return false;
}

bool BridgeTrap(CArmCpu& aCpu, THle& aHle, const char* aDll, uint32_t aOrd)
{
	gCurHle = &aHle;
	TBridgeDll* d = 0;
	for (int i = 0; i < 2 && !d; ++i)
		if (gDll[i].iNativeOn && SameDll(aDll, gDll[i].iName)) d = &gDll[i];
	if (!d || aOrd < 1 || aOrd > d->iCount)
		return false;
	const uint32_t row = aOrd - 1;
	const TBridgeExport& e = d->iRows[row];
	RegisterPrimary(aCpu);
	if (d == &gDll[0] && IsLeaveRow(e.iName) && LeaveOverride(aCpu, e.iName, NativeOf(d, row, aHle)))
		return true;
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
	if (gTrace)
	{
		char line[200];
		snprintf(line, sizeof(line), "[proteus] bridge %s (%x,%x,%x,%x)", e.iName, aCpu.R[0], aCpu.R[1], aCpu.R[2], aCpu.R[3]);
		Say(aHle, "%s", line);
	}
	PatchArgVtables(aCpu, *d, row);
	const uint32_t lr = aCpu.R[14];
	e.iCall(aCpu, d->iFn[row]);
	aCpu.R[14] = lr;
	aCpu.SetReturn();
	return true;
}
}

namespace proteus
{
// the name a native ordinal of aDll is bound under (the host tests' fake native tables key on names)
const char* BridgeNativeName(const char* aDll, uint32_t aNativeOrdinal)
{
	for (int i = 0; i < 2; ++i)
		if (SameDll(aDll, gDll[i].iName))
			for (uint32_t r = 0; r < (i ? KBridge_efsrv_Count : KBridge_euser_Count); ++r)
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
}

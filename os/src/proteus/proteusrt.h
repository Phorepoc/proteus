// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusrt.h -- OURS (Proteus P0): the portable runtime around the interpreter: an E32Image loader for ARM EXEs (uncompressed, ARMv5/v6 ARM-mode) into the guest region,
// and the "native system" the program calls into -- for P0 a tiny HLE of euser (RDebug::Printf, User::Exit, process/heap init) bound by ORDINAL through the ARM EABI
// euser export table. The same boundary becomes the executive-call table (SVC) and the real ARM euser in P1/P2.
#ifndef PROTEUS_RT_H
#define PROTEUS_RT_H

#include "armcpu.h"

namespace proteus
{
enum { KMaxDllRefs = 64 };		// imported DLLs per module
struct TLoaded
{
	uint32_t iCpu, iUid3, iFlags;
	uint32_t iLoadAddr;			// guest address of the code (the image's code_base maps here)
	uint32_t iEntry;			// guest address of the entry point
	uint32_t iDelta;			// loadAddr - codeBase
	uint32_t iEnd;				// first guest address after code + data + bss
	int iNumImports;
	char iDll[KMaxDllRefs][48];	// import DLL names (trap addresses encode the index)
	char iError[96];
	// P2: DLL linking
	char iName[40];				// base name ("proteusdll.dll"; empty for the EXE)
	char iDrive;				// the drive the file was found on (RLibrary::FileName), 0 = unknown
	uint32_t iUid1;
	uint32_t iSecureId, iVendorId, iCaps[2];	// the header's SSecurityInfo (what the program declares)
	uint32_t iProcPriority;		// the EXE header's iProcessPriority (a TProcessPriority value, 0 = not set)
	uint32_t iHeapMin, iHeapMax, iStackSize;	// EXE header (real euser: the SStdEpocThreadCreateInfo on the initial stack)
	uint32_t iExportDir, iExportCount;	// guest address of the (relocated) export directory
	uint32_t iExceptionDescriptor;	// guest address of the C++ unwinder's exception descriptor (E32ImageHeaderV::iExceptionDescriptor), 0 = none
	int iModule;				// index in the TProcess (trap addresses encode it)
	int iDllModule[KMaxDllRefs];			// per import DLL: the guest module it binds to, -1 = HLE (trap)
};

// P2: the modules of one guest process -- the EXE (module 0) and the ARM DLLs it imports, transitively, laid out one after another in the guest region.
enum { KMaxModules = 512 };
// find an ARM DLL by its base name ("proteusdll.dll"); returns the file bytes (owned by the embedder, alive while the process runs) or NULL = not an
// ARM DLL we have (the import then stays an HLE trap)
typedef const uint8_t* (*TFindDllFn)(const char* aName, uint32_t& aLen, char& aDrive, void* aCtx);	// aDrive: the drive letter it was found on
struct TProcess
{
	TLoaded iMod[KMaxModules];
	int iCount;
	char iError[96];
	// HLE import slots: a trap address is KTrapBase | slot << 2 (4-aligned: an ARM-state BX to it clears bits 1:0); a slot = module << 22 | DLL ref << 16 | ordinal (malloc'd, FreeProcess)
	uint32_t* iSlots;
	uint32_t iSlotCount, iSlotMax;
	bool iRealEuser;			// P2: set before LoadProcess = euser imports bind to a real ARM euser.dll (via aFind) + the SWI executive
};
void FreeProcess(TProcess& aProcess);
// P2: guest RLibrary::Load (proteusexec.cpp's !Loader): module index or an error; aFirstNew = the first newly loaded module
int LoadLibraryModule(CArmCpu& aCpu, TProcess& aProc, const char* aName, TFindDllFn aFind, void* aFindCtx, int& aFirstNew);
bool CallsEntry(CArmCpu& aCpu, const TLoaded& aModule);
// the order the loader runs entry points in: modules aFirst.. of aProc reachable from aRoot, each after the modules it imports
// (post-order, as the kernel's DCodeSeg list; a cycle runs in visit order). Returns the count written to aOrder (KMaxModules room).
int InitOrder(const TProcess& aProc, int aRoot, int aFirst, int* aOrder);
// describe a guest address: module+offset and the nearest export below it
void Where(const TProcess& aProcess, CArmCpu& aCpu, uint32_t aAddr, char* aOut, int aMax);
// loads aFile (an EXE) at aAt plus every non-euser DLL it imports that aFind can supply (transitively), and binds the imports by ordinal
bool LoadProcess(CArmCpu& aCpu, const uint8_t* aFile, uint32_t aLen, uint32_t aAt, TFindDllFn aFind, void* aFindCtx, TProcess& aOut);

// Loads the E32 image in aFile into the guest region of aCpu at aAt (page aligned, inside the region). false + iError on a problem.
bool LoadE32(CArmCpu& aCpu, const uint8_t* aFile, uint32_t aLen, uint32_t aAt, TLoaded& aOut);

typedef void (*TLogFn)(const char* aLine, void* aCtx);

// P2: the IPC bridge -- guest client sessions become sessions of THIS (native x32) process with the native servers. Guest memory is host
// memory (ILP32) and TIpcArgs / descriptors have the same layout, so message arguments pass through unchanged; the native kernel completes
// the guest's TRequestStatus directly and signals this thread (guest WaitForAnyRequest = the native one). Supplied by the embedder.
struct TIpcBridge
{
	void* iCtx;
	int (*iSessionCreate)(void* aCtx, const char* aName, int aSlots);					// -> a bridge handle (> 0) or an error
	int (*iSend)(void* aCtx, uint32_t aHandle, int aFunction, uint32_t aArgs, uint32_t aStatus, bool aSync);	// aSync = SessionSendSync (the thread's own message slot); else async (aStatus 0 = blind)
	bool (*iOwns)(void* aCtx, uint32_t aHandle);
	int (*iClose)(void* aCtx, uint32_t aHandle);
	int (*iShare)(void* aCtx, uint32_t aHandle, int aMode);
	void (*iWaitAny)(void* aCtx);
	void (*iSignal)(void* aCtx, int aCount);
	// native kernel objects: Publish & Subscribe (EExecProperty*), OpenObject by name (chunks, mutexes, semaphores) and the calls on those
	// handles (Chunk*, Mutex*, Semaphore*, HandleClose). r0-r3 in, r0 out; false = not the embedder's (the guest-side emulation runs).
	// An opened chunk becomes a memory window of aCpu (CArmCpu::AddWindow).
	bool (*iKernel)(void* aCtx, const char* aExec, uint32_t* aR, CArmCpu& aCpu);
	// guest threads: wait for a request signal at most aMicroseconds (true = one arrived and was taken, false = timed out); a monotonic clock
	bool (*iWaitAnyFor)(void* aCtx, int aMicroseconds);
	uint64_t (*iNowUs)(void* aCtx);
	uint32_t (*iNativeExport)(void* aCtx, const char* aDll, int aOrdinal);	// a native DLL's export (the GL thunks), 0 = none
	bool (*iPoll)(void* aCtx);		// a request signal that has already arrived: taken (true), or none pending (false); never blocks
	bool (*iSyncDone)(void* aCtx);	// the last iSend completed the guest's status itself (CompleteGuest), no native signal pending for it: true once, then cleared
	// native-hosted threads: a guest thread whose body the system runs on the native side (the MMF controller thread: the client creates it
	// in its own process and talks to it only through a private server's session). iHostThread is asked at the thread's creation with the module
	// its entry function lies in and the creation argument (a guest address); true = hosted: it is never scheduled. iHostStart is its first
	// Resume (the thread "runs": the result is what the thread rendezvous with, KErrNone = up); iHostExit says whether it has ended (type, reason);
	// iHostKill is a Kill of it. iSessionFromServer is SessionCreateFromHandle on the server handle iHostStart handed the guest.
	bool (*iHostThread)(void* aCtx, const char* aModule, int aThread, uint32_t aArg);
	int (*iHostStart)(void* aCtx, int aThread);
	bool (*iHostExit)(void* aCtx, int aThread, int* aType, int* aReason);
	void (*iHostKill)(void* aCtx, int aThread, int aReason);
	int (*iSessionFromServer)(void* aCtx, uint32_t aServerHandle, int aSlots);
};

// P2: the executive state of a guest that runs the real ARM euser (proteusexec.cpp)
enum { KStackBytes = 1 << 20, KHandleChunk0 = 0x00C10000, KHandleObj = 0x00D00000, KHandleLib0 = 0x00E40000, KHandleLoader = 0x00E3F000,
	KHandleThread0 = 0x00F00000 };		// guest thread i = KHandleThread0 + i (positive: euser takes a negative handle as an error)

// guest threads: each has its own registers, stack + TLocalThreadData, request semaphore and the per-thread executive state (heap, trap
// handler, active scheduler, TLS). They run on the one native thread, switched cooperatively whenever the running one blocks (WaitForAnyRequest
// with no signal, a mutex / semaphore / condition-variable wait, exit): the kernel's scheduling decisions without preemption.
// (the sync object table grows as the guest needs it, up to what a handle can name: KHandleObj + index stays below the bridge handles at 0xE00000)
enum { KMaxThreads = 32, KSyncInit = 512, KMaxSync = 0xF0000, KMaxLogons = 8, KMaxReqs = 128 };
enum TThreadState { EThreadFree, EThreadSuspended, EThreadReady, EThreadWaitAny, EThreadWaitMutex, EThreadWaitSem, EThreadWaitCond, EThreadDead };
struct TGuestThread
{
	int iState;
	uint32_t iId;
	// saved CPU state (the running thread's is in the CArmCpu)
	uint32_t iR[16], iN, iZ, iC, iV, iT, iGE, iTpidrurw, iTpidruro, iFpscr, iFpexc;
	uint64_t iD[32];
	// saved per-thread executive state (the running thread's is in TExecState)
	uint32_t iHeap, iTrapHandler, iActiveScheduler, iTrapFrame;
	uint32_t iTlsKey[64], iTlsVal[64];
	int iTlsCount;
	int iRequests;				// the request semaphore count (DThread::iRequestSemaphore)
	int iWaitObj;				// EThreadWaitMutex / Sem / Cond: the sync object
	int iCondMutex, iCondCount;	// EThreadWaitCond: the mutex to take back on wake-up, and its hold count
	int32_t iWaitResult;		// the result a wait returns once the mutex is taken back (a timed-out condvar wait: KErrTimedOut)
	uint32_t iWaitSeq;			// FIFO order of the waiters of one object
	uint64_t iDeadline;			// timed wait: host microseconds (TIpcBridge::iNowUs), 0 = none
	uint64_t iAfterAt;			// the thread's own timer (Exec::After / AfterHighRes: User::After, User::AfterHighRes): host microseconds ...
	uint32_t iAfterStatus;		// ... and the TRequestStatus it completes (0 = no timer pending)
	uint64_t iRan;				// instructions it has executed (accounted when it stops running)
	uint64_t iReadyAt;			// host microseconds when it last became ready (a wake-up), 0 = it has been picked since
	uint64_t iWakeSum, iWakeMax; uint32_t iWakeN, iWakeSlow;	// wake-up latency (ready -> running) statistics: count, sum / max in us, those over 10 ms
	int iExitType, iExitReason;
	char iExitCategory[17];
	uint32_t iLogonStatus[KMaxLogons];
	int iLogonOwner[KMaxLogons];
	bool iLogonRendezvous[KMaxLogons];
	int iStackChunk;			// the chunk holding its stack (-1 = the main thread's region stack)
	bool iStackFree;			// dead: the stack chunk may be reused by a new thread
	int iThreadPri;				// the kernel's DThread::iThreadPriority: -8..-2 relative to the process priority, >= 0 absolute (TThrdPriority, kern_priv.h)
	int iAbsPri;				// the absolute priority 0..63 the scheduler runs by (DThread::CalcDefaultThreadPriority); higher runs first
	bool iHosted, iHostStarted;	// the body runs on the native side (TIpcBridge::iHostThread): never scheduled
};
struct TSyncObj
{
	int iType; int iCount; int iHolder;
	int iRefs;			// handles to it (RHandleBase::Duplicate adds one, Close removes one; none left = the slot is free: iType 0)
	// a message queue (type 4: iCount = messages queued, iHolder = the head slot); the requests of NotifySpaceAvailable / NotifyDataAvailable
	uint8_t* iData; int iSlots, iLen; uint32_t iSpaceStatus, iDataStatus; int iSpaceThread, iDataThread;
	char iName[48];		// a global object's name (RSemaphore::CreateGlobal ...), empty = anonymous
};		// 1 mutex (iCount = hold count, iHolder = thread), 2 semaphore (iCount), 3 condition variable, 4 message queue
struct TExecState
{
	uint32_t iHeap, iTrapHandler, iActiveScheduler, iTrapFrame;
	uint32_t iTlsKey[64], iTlsVal[64];
	int iTlsCount;
	uint32_t iChunkBase[64], iChunkMax[64];
	uint8_t* iChunkPages[64];	// committed-page map (1 byte per 4 KB page, malloc'd; leaked with the process)
	uint32_t iChunkAtt[64];		// TChunkCreate::iAtt (low nibble: 0 normal, 1 double-ended, 2 disconnected)
	int iChunkCount;
	uint32_t iChunkTop, iLowWater;		// chunks grow down from below the stack, not under iLowWater (the end of the loaded modules)
	uint32_t iCalls;
	int iLibFirstNew;			// guest RLibrary::Load: the first module whose entry point LibraryAttach still has to report
	// guest threads (RunExe allocates the tables; leaked with the process)
	TGuestThread* iThread;
	int iThreadCount, iCur;
	TSyncObj* iSync;
	int iSyncCount, iSyncCap;		// slots in use (high water mark) and allocated
	uint32_t iWaitSeq;
	uint32_t iReentry;			// EFastExecSetReentryPoint: where a new thread starts (0 = the EXE's entry point, DProcess::iReentryPoint's default)
	// the requests guest threads handed to native servers / timers: a native signal belongs to the thread whose TRequestStatus completed
	uint32_t iReqStatus[KMaxReqs];
	int iReqThread[KMaxReqs];
	bool iReqDone[KMaxReqs];		// the request completed and its guest status was reused for a newer request before the signal was credited: it is owed one signal
	uint64_t iWaitUs;			// microseconds spent blocked natively (Reschedule: nothing ready)
	uint64_t iSliceStart;		// CArmCpu::iSteps when the running thread started its turn
	int iProcPri;				// DProcess::iPriority (TProcPriority 0..7, kern_priv.h): the row of the priority table
	uint32_t iSyncSendStatus;	// the outstanding SessionSendSync (the native thread has one synchronous message slot for all guest threads)
};
struct THle
{
	TLoaded* iImage;
	TProcess* iProcess;			// P2: when set, trap addresses carry the module index
	TLogFn iLog;
	void* iLogCtx;
	int iUnimplemented;			// count of imports that were called but not emulated
	TExecState iExec;			// P2 (real euser)
	TIpcBridge* iIpc;			// P2: NULL = no servers (sessions fail with KErrNotSupported)
	const char* iExePath;		// P2: the guest EXE's full name (ProcessFileName etc.; NULL = "z:\sys\bin\guest.exe")
	uint32_t iRomHeader;
	TFindDllFn iFind;			// P2: guest RLibrary::Load finds ARM DLLs like LoadProcess did
	void* iFindCtx;		// P2: the native ROM header address (EFastExecRomHeaderAddress; the ROM is a memory window), 0 = none
	uint32_t iThreadId;		// the guest thread's id (TLocalThreadData::iThreadId: RThread().Id() on an ARMv6K+ euser)
};

// a SWI from the guest (real ARM euser): emulate the executive call; false = not an executive call we know (the guest stops)
bool ExecCall(CArmCpu& aCpu, THle& aHle, uint32_t aImm);
// log lr + a heuristic stack backtrace (module+offset)
void Backtrace(CArmCpu& aCpu, THle& aHle);

// install the HLE hooks on aCpu (trap + svc, and the time slice)
void InstallHle(CArmCpu& aCpu, THle& aHle);

// instructions of a time slice: after that many the running guest thread yields to the next ready one; 0 = cooperative scheduling only.
// ON since every native request signal is credited exactly (TExecState::iReqDone, NoteRequest, TIpcBridge::iSyncDone): see docs/sparkle-gl-run.md
static const uint64_t KSliceInstructions = 1u << 17;
// the end of a time slice (CArmCpu::iOnSlice): native request completions that arrived are credited to their threads, timed waits that
// ran out expire, and the next ready thread (round robin) runs
void Preempt(CArmCpu& aCpu, THle& aHle);
// the absolute priorities (DThread::CalcDefaultThreadPriority): the process priority from the EXE header, thread 0 at Normal. Call once the thread table exists.
void InitPriorities(THle& aHle);
// "cur=i ran(M): i:n ..." (instructions each guest thread has executed), into aOut
void ThreadStats(CArmCpu& aCpu, THle& aHle, char* aOut, int aMax);

// the name of an euser ordinal in the ARM EABI export table ("" if unknown)
const char* EuserName(uint32_t aOrdinal);

// a line in the guest's log (fmt takes up to two strings)
void Say(THle& h, const char* fmt, const char* a = "", const char* b = "");

// aLen bytes copied into guest memory Proteus keeps for itself (a reserved chunk, committed as it fills; never freed):
// the guest address, 0 = no memory (proteusexec.cpp)
uint32_t GuestBytes(CArmCpu& aCpu, TExecState& aExec, const void* aSrc, uint32_t aLen);

// proteusgl.cpp: the imports of the Khronos DLLs run on the native ones. IsGlDll = an import of one of them; GlTrap = call
// the native export of aOrd (false = not in the tables); GlTrapSlot = an import slot eglGetProcAddress made (false = not one)
bool IsGlDll(const char* aImport);
bool GlTrap(CArmCpu& aCpu, THle& aHle, const char* aDll, uint32_t aOrd);
bool GlTrapSlot(CArmCpu& aCpu, THle& aHle, uint32_t aSlotValue);
// bridge.cpp (rung R1): the imports of the DLLs bridge.cfg lists as native run on the native x32 export of the same name.
// BridgeConfigure = (re)read the cfg text; BridgeNative = an import of such a DLL; BridgeTrap = call it (false = not bridged: the HLE runs);
// BridgeMapWanted = the host range to make guest memory as it is (the "map" line; false = off)
void BridgeConfigure(const char* aText);
bool BridgeNative(const char* aImport);
bool BridgeTrap(CArmCpu& aCpu, THle& aHle, const char* aDll, uint32_t aOrd);
void BridgeSetLookup(uint32_t (*aFn)(void* aCtx, const char* aDll, int aOrdinal), void* aCtx);	// before LoadProcess: where native DATA imports are looked up
bool BridgeDataImport(const char* aDll, uint32_t aOrd, uint32_t& aAddr);
bool BridgeDataAlias(const char* aDll, uint32_t aOrd, const char*& aArmDll, uint32_t& aArmOrd);	// that DATA import is an ARM DLL's export instead
bool BridgeMapWanted(uint32_t& aLo, uint32_t& aHi);
uintptr_t BridgeStubFn(uint32_t aAddr);	// (host tests) the callable function behind a reverse stub address
void BridgeSetResolver(uintptr_t (*aFn)(uint32_t));	// (host tests)
// native threads for guest threads (R1): what the embedder provides -- the current native thread's id, guest memory for a guest thread's stack
// (a guest address; on the board the native heap is guest memory), and the translator per CPU (null = the interpreter)
struct TBridgeThreads
{
	uint32_t (*iThreadId)();
	uint32_t (*iAlloc)(uint32_t aSize);
	void (*iFree)(uint32_t aAddr);
	TArmJit* (*iJitNew)(CArmCpu& aCpu);
	void (*iJitDelete)(TArmJit* aJit);
	void (*iLog)(const char* aLine);
	void (*iTrapLeave)(uint32_t aHandler, int32_t aReason);
	int32_t (*iTrapped)(void (*aFn)(void*), void* aArg);	// run aFn under a NATIVE TRAP: its leave code, 0 = none (null = no trapping)	// (host tests) the native TTrapHandler::Leave; null = call it through its vtable
};
void BridgeSetThreads(const TBridgeThreads& aThreads);
bool BridgeIsGuestThread(const CArmCpu& aCpu);			// a CPU of a native guest thread (not the process's primary): the HLE is not for it
// objects across the boundary: the guest function behind a reverse stub (0 = none); the guest called a native address (a native object's
// virtual function): call it with r0-r3 + two stack words, result in r0/r1, and return lr (0 = the bridge is off)
uint32_t BridgeStubTarget(uint32_t aPc);
uint32_t BridgeNativeCall(CArmCpu& aCpu, uint32_t aPc);
uint32_t BridgeOnForeignPc(CArmCpu& aCpu, uint32_t aPc, void* aCtx);	// the two above, for an embedder with no other foreign-pc mapping
const char* BridgeNativeName(const char* aDll, uint32_t aNativeOrdinal);	// (host tests)
// eglCreateWindowSurface(dpy, config, window, attribs) with the guest's RWindow (proteusglwin.cpp)
extern int gExecTraceWindow;		// > 0: log the next that many executive calls
bool GlWindowSurface(CArmCpu& aCpu, THle& aHle, uint32_t aNativeFn);

// run an ARM EXE from entry: sets sp (top of the guest region), r4 = 0 (first thread), lr = return-to-host, and runs until it halts
void RunExe(CArmCpu& aCpu, THle& aHle, uint64_t aMaxSteps);
}

#endif

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
	uint32_t iHeapMin, iHeapMax, iStackSize;	// EXE header (real euser: the SStdEpocThreadCreateInfo on the initial stack)
	uint32_t iExportDir, iExportCount;	// guest address of the (relocated) export directory
	uint32_t iExceptionDescriptor;	// guest address of the C++ unwinder's exception descriptor (E32ImageHeaderV::iExceptionDescriptor), 0 = none
	int iModule;				// index in the TProcess (trap addresses encode it)
	int iDllModule[KMaxDllRefs];			// per import DLL: the guest module it binds to, -1 = HLE (trap)
};

// P2: the modules of one guest process -- the EXE (module 0) and the ARM DLLs it imports, transitively, laid out one after another in the guest region.
enum { KMaxModules = 256 };
// find an ARM DLL by its base name ("proteusdll.dll"); returns the file bytes (owned by the embedder, alive while the process runs) or NULL = not an
// ARM DLL we have (the import then stays an HLE trap)
typedef const uint8_t* (*TFindDllFn)(const char* aName, uint32_t& aLen, char& aDrive, void* aCtx);	// aDrive: the drive letter it was found on
struct TProcess
{
	TLoaded iMod[KMaxModules];
	int iCount;
	char iError[96];
	// HLE import slots: a trap address is KTrapBase | slot << 1; a slot = module << 22 | DLL ref << 16 | ordinal (malloc'd, FreeProcess)
	uint32_t* iSlots;
	uint32_t iSlotCount, iSlotMax;
	bool iRealEuser;			// P2: set before LoadProcess = euser imports bind to a real ARM euser.dll (via aFind) + the SWI executive
};
void FreeProcess(TProcess& aProcess);
// P2: guest RLibrary::Load (proteusexec.cpp's !Loader): module index or an error; aFirstNew = the first newly loaded module
int LoadLibraryModule(CArmCpu& aCpu, TProcess& aProc, const char* aName, TFindDllFn aFind, void* aFindCtx, int& aFirstNew);
bool CallsEntry(CArmCpu& aCpu, const TLoaded& aModule);
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
};

// P2: the executive state of a guest that runs the real ARM euser (proteusexec.cpp)
enum { KStackBytes = 1 << 20, KHandleChunk0 = 0x00C10000, KHandleObj = 0x00D00000, KHandleLib0 = 0x00E40000, KHandleLoader = 0x00E3F000 };
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

// install the HLE hooks on aCpu (trap + svc)
void InstallHle(CArmCpu& aCpu, THle& aHle);

// the name of an euser ordinal in the ARM EABI export table ("" if unknown)
const char* EuserName(uint32_t aOrdinal);

// run an ARM EXE from entry: sets sp (top of the guest region), r4 = 0 (first thread), lr = return-to-host, and runs until it halts
void RunExe(CArmCpu& aCpu, THle& aHle, uint32_t aMaxSteps);
}

#endif

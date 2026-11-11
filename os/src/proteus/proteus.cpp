// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteus.cpp -- OURS (Proteus P0, docs/phorepoc/research/proteus-x86.md): proteus.exe, the first Proteus component on the x86/x32 board: a USER-SIDE loader + ARM interpreter that
// runs a real ARM Symbian EXE (an E32Image with CPU id ARMv5) inside one x32 process. proteus.exe [<arm exe>] (default z:\sys\bin\arm\proteushello.exe: the ARM files live in the
// per-arch directory \sys\bin\arm\ of the design). The guest's memory is one block of this process's heap (guest address == host address under ILP32); the ARM code's euser imports are
// emulated by ordinal (proteusrt.cpp). Prints "PH: proteus ..." lines. P0 stand-in for the loader hook (f32image.h iCpuIdentifier) + dynarmic of P1.
#include <stdio.h>
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>
#include <e32property.h>
#include <e32msgqueue.h>
#include <w32std.h>
#include <e32rom.h>
#include <e32svr.h>
#include <e32hal.h>
#include <apacmdln.h>
#include <mmf/common/mmfcontrollerframework.h>
#include <string.h>
#include "armcpu.h"
#include "armjit.h"
#include "proteusrt.h"
#include "../proteusimg/proteusimg.h"

static void SigTrace(const char* aWhat, TUint aStatus, TInt aValue);

static void ResetDestroy(TAny* aArray);
_LIT(KDefaultExe, "z:\\sys\\bin\\arm\\proteushello.exe");
// the guest region: reserved address space, committed as the guest uses it (modules as they load, the main stack, chunk pages as the guest
// commits them) -- a real app's heap reserves its EPOCHEAPSIZE maximum (PyS60: 32 MB; Sparkle: 128 MB, which euser doubles for the heap chunk)
// and commits a little of it. As much as a process's local address space gives (an ARM process has ~1 GB): the reservation costs no RAM.
const TInt KGuestBytesMax = 1 << 30, KGuestBytesMin = 64 << 20;
// native services map into this process after the guest region is made, all in the same local address space: the font and bitmap
// server's chunks (192 MB), Mesa's chunk heap (128 MB), the JIT's code (32 MB) and the GL GPU channel's 256 MB pool (the PC board layer symvmw:
// Mesa's svga driver on a VM's host GPU; without one free range that large it fails with KErrNoMemory and GL falls back to the software
// renderer). The guest region leaves this much of the largest reservation the kernel grants free for them.
const TInt KNativeReserveBytes = 320 << 20;

// CArmCpu::TCommitFn: commit / decommit host pages of the guest region (a disconnected chunk). A range may overlap pages committed
// before (a module's last page and the next module's first): then it goes page by page and those count as done.
static bool CommitGuest(uint32_t aOffset, uint32_t aSize, bool aCommit, void* aCtx)
	{
	RChunk& c = *(RChunk*)aCtx;
	TInt r = aCommit ? c.Commit(aOffset, aSize) : c.Decommit(aOffset, aSize);
	if (r == KErrAlreadyExists && aCommit)
		{
		r = KErrNone;
		for (uint32_t o = aOffset; o < aOffset + aSize && r == KErrNone; o += 0x1000)
			{
			r = c.Commit(o, 0x1000);
			if (r == KErrAlreadyExists) r = KErrNone;
			}
		}
	if (r != KErrNone)
		RDebug::Printf("PH: proteus %scommit guest +%x %x -> %d", aCommit ? "" : "de", aOffset, aSize, r);
	return r == KErrNone;
	}

// a 16-bit name for RDebug::Printf's %S (an 8-bit descriptor there: a 16-bit one prints its first character)
struct TLog8 : public TBuf8<KMaxFileName>
	{
	explicit TLog8(const TDesC& a) { Copy(a.Left(KMaxFileName)); }
	const TDesC8* P() const { return this; }
	};

static void Log(const char* aLine, void*)
	{
	RDebug::Printf("PH: proteus guest> %s", aLine);
	}

// an ARM image, read-only. Directly when this process may (proteus.exe; a program declaring AllFiles), else through the image broker:
// the loader gives cpuhandler_arm.exe the program's capabilities, and reading \sys\ needs AllFiles (proteusimg.cpp)
static TBool gViaBroker = EFalse;
static TInt OpenImage(RFs& aFs, const TDesC& aPath, RFile& aFile)
	{
	if (gViaBroker)
		{
		TEntry e;
		if (aFs.Entry(aPath, e) == KErrNotFound)
			return KErrNotFound;		// the search probes many places: no broker round trip for a missing file
		const TInt b = ProteusImgOpen(aPath, aFile);
		RDebug::Printf("PH: proteus image broker -> %d", b);
		return b;
		}
	const TInt r = aFile.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	return r == KErrPermissionDenied ? ProteusImgOpen(aPath, aFile) : r;
	}

// P2: the ARM DLLs a guest imports come from the per-arch directory on the EXE's drive, then Z: (buffers live until the process ends)
struct TFindCtx { RFs* iFs; TFileName iDir; RPointerArray<HBufC8>* iBufs; };
static const uint8_t* FindDll(const char* aName, uint32_t& aLen, char& aDrive, void* aCtx)
	{
	TFindCtx& c = *(TFindCtx*)aCtx;
	TBuf<64> name;
	name.Copy(TPtrC8((const TUint8*)aName));
	// the EXE's per-arch directory first, then the per-arch directory \sys\bin\arm\ of every drive in the loader's order (Y: .. A:, Z:;
	// a ROM built with this layer has it on Z:, a PC image with Proteus installed from its SIS has an ARM closure staged on C:), then
	// \sys\bin\ of every drive, where a SIS installs an app's ARM DLLs next to the native ones: only a file whose E32Image header
	// names an ARM CPU (offset 122) is taken there
	TFileName path;
	for (TInt i = 0; i < 1 + 26 + 26; ++i)
		{
		if (i == 0) path = c.iDir;
		else
			{
			const TInt k = (i - 1) % 26;
			TChar ch;
			if (RFs::DriveToChar(k < 25 ? EDriveY - k : EDriveZ, ch) != KErrNone) continue;
			path.SetLength(0);
			path.Append(ch);
			path.Append(i <= 26 ? _L(":\\sys\\bin\\arm\\") : _L(":\\sys\\bin\\"));
			}
		path.Append(name);
		RFile f;
		if (OpenImage(*c.iFs, path, f) != KErrNone)
			continue;
		TInt size = 0;
		HBufC8* b = f.Size(size) == KErrNone ? HBufC8::New(size) : NULL;
		TPtr8 bp(b ? b->Des() : TPtr8(NULL, 0));
		const TInt r = b ? f.Read(0, bp) : KErrNoMemory;
		f.Close();
		if (r == KErrNone && i > 26 && (bp.Length() < 124 || (bp[122] | (bp[123] << 8)) < 0x2000 || (bp[122] | (bp[123] << 8)) > 0x2002))
			{
			delete b;					// a native DLL of that name
			continue;
			}
		if (r != KErrNone || c.iBufs->Append(b) != KErrNone)
			{
			delete b;
			return NULL;
			}
		TBuf8<KMaxFileName> path8;
		path8.Copy(path);
		RDebug::Printf("PH: proteus DLL %S (%d bytes)", &path8, size);
		aLen = (uint32_t)size;
		aDrive = (char)path[0];		// the per-arch directories count as their drive (the arm subdirectory of C: is C:)
		return b->Ptr();
		}
	RDebug::Printf("PH: proteus DLL %s: no ARM file (stays HLE)", aName);
	return NULL;
	}

// P2: the IPC bridge (proteusrt.h TIpcBridge). A guest session = one native RSessionBase of this process. The guest's RSessionBase::CreateSession
// is SessionCreate (name, slots) + an EConnect message carrying the TVersion: the native session is created at that EConnect, with the
// guest's TRequestStatus. Every other message goes to the native server with the guest's TIpcArgs as they are (same layout, same memory).
class RBridgeSession : public RSessionBase
	{
public:
	TInt Connect(const TDesC& aName, TInt aVersion, TInt aSlots, TRequestStatus* aStatus)
		{
		TVersion v;
		Mem::Copy(&v, &aVersion, sizeof(v));
		return CreateSession(aName, v, aSlots, EIpcSession_Unsharable, NULL, aStatus);
		}
	void Async(TInt aFunction, const TIpcArgs& aArgs, TRequestStatus& aStatus) const { SendReceive(aFunction, aArgs, aStatus); }
	TInt Blind(TInt aFunction, const TIpcArgs& aArgs) const { return Send(aFunction, aArgs); }
	TInt Sync(TInt aFunction, const TIpcArgs& aArgs) const { return SendReceive(aFunction, aArgs); }
	};

enum { KBridgeHandle0 = 0x00E00000, KMaxBridge = 64 };
struct TBridge
	{
	RBridgeSession iSession[KMaxBridge];
	TBuf<128> iName[KMaxBridge];
	TInt iSlots[KMaxBridge];
	TBool iUsed[KMaxBridge], iConnected[KMaxBridge];
	TBool iNative[KMaxBridge];					// a session handle a server handed the guest in a reply (RMessage2::Complete(handle)): a kernel handle of this process, adopted at its first use
	RProperty iProp[KMaxBridge];				// guest RProperty handles = KPropHandle0 + i
	TBool iPropUsed[KMaxBridge];
	RChunk iObj[KMaxBridge];					// P2: kernel objects opened by name (chunk / mutex / semaphore: all an RHandleBase), KObjHandle0 + i
	TInt iObjType[KMaxBridge];					// TObjectType, 0 = free slot (EThread is never opened here)
	TUint32 iObjNative[KMaxBridge];				// a kernel object a server handed the guest in a reply (RMessage2::Complete(handle)): this process's handle, the guest keeps using that value; 0 = opened/created here (KObjHandle0 + i)
	// the guest's file-server identity: \private\<its SecureId>\ (sessions start there, and the private-path calls answer it --
	// BridgeFsPrivate; the same as the server's own answers when the loader gave this process the program's SecureId)
	TUint32 iSid;
	TBool iPathSet[KMaxBridge];
	TInt iCtl[KMaxBridge];							// a session with a natively hosted MMF controller: its slot in gCtl (-1 = a session of a server by name)
	RFs* iFs;
	};

// The guest's window-server session is a native RWsSession of this process (its first one): the guest's messages go to the
// server on that session's handle as any other, and native code can address the guest's windows -- an RWindow of gWs that
// carries the guest window's server handle (GlWindowSurface: the native EGL draws into the guest's window). gWs's
// Connect sends the EWservMessInit; the guest's own Init is answered with the same client handle.
static RWsSession gWs;
static TInt gWsIndex = -1;

static TInt BridgeWsConnect(TBridge& b, TInt i)
	{
	const TInt r = gWs.Connect();
	if (r != KErrNone)
		return r;
	b.iSession[i].SetHandle(gWs.Handle());
	gWsIndex = i;
	return KErrNone;
	}

// a native RWindow over a guest window: the guest RWindow begins with its server handle (MWsClientClass::iWsHandle; no
// virtual functions in RWindowTreeNode's chain). Never closed: the window is the guest's.
class TGuestWindow : public RWindow
	{
public:
	TGuestWindow(RWsSession& aWs, TInt32 aHandle) : RWindow(aWs) { iWsHandle = aHandle; }
	void Rebind(TInt32 aHandle) { iWsHandle = aHandle; }		// (a guest RWindow at an address used before)
	};

bool proteus::GlWindowSurface(proteus::CArmCpu& aCpu, proteus::THle& aHle, uint32_t aNativeFn)
	{
	// eglCreateWindowSurface(display, config, window, attributes): the window = the guest's RWindow*
	const uint32_t guestWin = aCpu.R[2];
	if (gWsIndex < 0 || !aCpu.Valid(guestWin, 8))
		{
		proteus::Say(aHle, "[proteus] eglCreateWindowSurface: no guest window-server session or a bad window -> EGL_NO_SURFACE");
		aCpu.R[0] = 0;
		aCpu.SetReturn();
		return true;
		}
	static uint32_t guest[16];
	static TGuestWindow* native[16];
	static TInt count;
	TInt k = 0;
	while (k < count && guest[k] != guestWin) ++k;
	if (k == count)
		{
		if (count == 16 || !(native[k] = new TGuestWindow(gWs, (TInt32)aCpu.Read32(guestWin))))
			{
			aCpu.R[0] = 0;
			aCpu.SetReturn();
			return true;
			}
		guest[k] = guestWin;
		++count;
		}
	native[k]->Rebind((TInt32)aCpu.Read32(guestWin));
	typedef uint32_t (*F)(uint32_t, uint32_t, uint32_t, uint32_t);
	aCpu.R[0] = ((F)(uintptr_t)aNativeFn)(aCpu.R[0], aCpu.R[1], (uint32_t)(uintptr_t)native[k], aCpu.R[3]);
	{
		char line[160];
		snprintf(line, sizeof(line), "[proteus] GL eglCreateWindowSurface(window %x, ws handle %x) -> %x", guestWin, (unsigned)aCpu.Read32(guestWin), aCpu.R[0]);
		proteus::Say(aHle, "%s", line);
	}
	aCpu.SetReturn();
	return true;
	}
enum { KObjHandle0 = 0x00E20000 };

// EExecObjectNext: a native TFindXxx with the guest's match pattern and cursor (TFindHandle is the base at offset 0 of every TFindXxx,
// 16 bytes, the same layout on both sides); the cursor goes back to the guest
template <class T> static TInt FindNext(const TDesC& aMatch, TFindHandle& aCursor, TFullName& aFound)
{
	T f(aMatch);
	Mem::Copy(&f, &aCursor, sizeof(TFindHandle));
	const TInt e = f.Next(aFound);
	Mem::Copy(&aCursor, &f, sizeof(TFindHandle));
	return e;
}

static TInt ObjIndex(TBridge& b, uint32_t aHandle)
	{
	const TInt i = (TInt)(aHandle & 0x7FFF);
	if ((aHandle & 0xFFFF0000u) == KObjHandle0 && i < KMaxBridge && b.iObjType[i])
		return i;
	for (TInt k = 0; k < KMaxBridge; ++k)		// (adopted kernel handles keep their own value)
		if (b.iObjType[k] && b.iObjNative[k] == aHandle)
			return k;
	return -1;
	}
enum { KPropHandle0 = 0x00E10000 };

static TInt PropIndex(TBridge& b, uint32_t aHandle)
	{
	const TInt i = (TInt)(aHandle & 0x7FFF);
	return (aHandle & 0xFFFF0000u) == KPropHandle0 && i < KMaxBridge && b.iPropUsed[i] ? i : -1;
	}

// Publish & Subscribe, natively: values and buffers are guest pointers = this process's memory
static bool BridgeProperty(void* aCtx, const char* aExec, uint32_t* r)
	{
	TBridge& b = *(TBridge*)aCtx;
	const TPtrC8 n((const TUint8*)aExec);
	const TUid cat = TUid::Uid((TInt)r[0]);
	if (n == _L8("EExecPropertyFindGetI")) { r[0] = (uint32_t)RProperty::Get(cat, r[1], *(TInt*)r[2]); return true; }
	if (n == _L8("EExecPropertyFindSetI")) { r[0] = (uint32_t)RProperty::Set(cat, r[1], (TInt)r[2]); return true; }
	if (n == _L8("EExecPropertyFindGetB"))
		{
		TPtr8 d((TUint8*)r[2], (TInt)r[3]);
		const TInt e = RProperty::Get(cat, r[1], d);
		r[0] = (uint32_t)(e == KErrNone ? d.Length() : e);
		return true;
		}
	if (n == _L8("EExecPropertyFindSetB")) { r[0] = (uint32_t)RProperty::Set(cat, r[1], TPtrC8((const TUint8*)r[2], (TInt)r[3])); return true; }
	if (n == _L8("EExecPropertyDefine"))		// (cat or KMaxTUint = own, key, TPropertyInfo*: TUint16 type, TUint16 size, read policy, write policy)
		{
		const TUint8* info = (const TUint8*)r[2];
		const TInt type = *(const TUint16*)info, size = *(const TUint16*)(info + 2);
		const TSecurityPolicy& rd = *(const TSecurityPolicy*)(info + 4);
		const TSecurityPolicy& wr = *(const TSecurityPolicy*)(info + 4 + sizeof(TSecurityPolicy));
		r[0] = (uint32_t)(r[0] == KMaxTUint ? RProperty::Define(r[1], type, rd, wr, size) : RProperty::Define(cat, r[1], type, rd, wr, size));
		return true;
		}
	if (n == _L8("EExecPropertyDelete")) { r[0] = (uint32_t)(r[0] == KMaxTUint ? RProperty::Delete(r[1]) : RProperty::Delete(cat, r[1])); return true; }
	if (n == _L8("EExecPropertyAttach"))
		{
		for (TInt i = 0; i < KMaxBridge; ++i)
			if (!b.iPropUsed[i])
				{
				const TInt e = b.iProp[i].Attach(cat, r[1]);
				if (e == KErrNone) { b.iPropUsed[i] = ETrue; r[0] = KPropHandle0 + i; } else r[0] = (uint32_t)e;
				return true;
				}
		r[0] = (uint32_t)KErrNoMemory;
		return true;
		}
	const TInt i = PropIndex(b, r[0]);
	if (i < 0)
		return false;
	RProperty& p = b.iProp[i];
	if (n == _L8("EExecPropertyGetI")) { r[0] = (uint32_t)p.Get(*(TInt*)r[1]); return true; }
	if (n == _L8("EExecPropertySetI")) { r[0] = (uint32_t)p.Set((TInt)r[1]); return true; }
	if (n == _L8("EExecPropertyGetB"))
		{
		TPtr8 d((TUint8*)r[1], (TInt)r[2]);
		const TInt e = p.Get(d);
		r[0] = (uint32_t)(e == KErrNone ? d.Length() : e);
		return true;
		}
	if (n == _L8("EExecPropertySetB")) { r[0] = (uint32_t)p.Set(TPtrC8((const TUint8*)r[1], (TInt)r[2])); return true; }
	if (n == _L8("EExecPropertySubscribe")) { p.Subscribe(*(TRequestStatus*)r[1]); return true; }
	if (n == _L8("EExecPropertyCancel")) { SigTrace("property-cancel", 0, 0); p.Cancel(); SigTrace("property-cancelled", 0, 0); return true; }
	return false;
	}

static TInt BridgeIndex(TBridge& b, uint32_t aHandle)
	{
	// (subsessions keep the session handle with KHandleNoClose 0x8000 set: RSubSessionBase::DoCreateSubSession)
	const TInt i = (TInt)(aHandle & 0x7FFF);
	if ((aHandle & 0xFFFF0000u) == KBridgeHandle0 && i < KMaxBridge && b.iUsed[i])
		return i;
	for (TInt k = 0; k < KMaxBridge; ++k)		// (adopted native session handles keep their own value)
		if (b.iUsed[k] && b.iNative[k] && b.iSession[k].Handle() == (TInt)(aHandle & ~0x8000u))
			return k;
	return -1;
	}

// a handle none of the guest's own ranges (chunks, sync objects, bridge sessions, properties, loader, libraries, threads: 0xC0..0xF7 in the
// instance field) can hold, and not the current-thread / process pseudo handles: a kernel handle that came from outside the guest
static TBool NativeLooking(uint32_t aHandle)
	{
	const uint32_t inst = aHandle >> 16;
	return aHandle && inst < 0xFFF0 && (inst < 0x00C0 || inst >= 0x00F8);
	}

// the guest's first message on a native session handle it was given: this process's handle, so the session object wraps it as is
static TInt AdoptNative(TBridge& b, uint32_t aHandle)
	{
	for (TInt i = 0; i < KMaxBridge; ++i)
		if (!b.iUsed[i])
			{
			b.iUsed[i] = ETrue;
			b.iConnected[i] = ETrue;
			b.iNative[i] = ETrue;
			b.iName[i] = _L("(native session handle)");
			b.iSlots[i] = -1;
			b.iSession[i].SetHandle((TInt)(aHandle & ~0x8000u));
			RDebug::Printf("PH: proteus IPC: guest uses the server-given session handle %x as bridge session %d", aHandle, i);
			return i;
			}
	return -1;
	}

static bool BridgeProperty(void* aCtx, const char* aExec, uint32_t* r);

// the guest program's command line: what its creator passed (the loader made this process's "<program> <command line>", MainL splits it)
static TBuf<KMaxFileName> gGuestCmd;

// a kernel handle the guest was given by a server's reply (RMessage2::Complete(handle) duplicates the server's object into this process): the
// guest uses it with the calls of the object's type (a chunk: Base, Size, ...). Taken over at its first use as that type; the guest keeps the
// handle's own value, and a chunk's memory becomes reachable for it (a window), as for a chunk opened by name.
static TInt AdoptObj(TBridge& b, proteus::CArmCpu& aCpu, uint32_t aHandle, TInt aType)
	{
	for (TInt i = 0; i < KMaxBridge; ++i)
		if (!b.iObjType[i])
			{
			b.iObj[i].SetHandle((TInt)aHandle);
			b.iObjType[i] = aType;
			b.iObjNative[i] = aHandle;
			if (aType == 2)		// TObjectType (u32std.h): 2 EChunk
				{
				RChunk& c = b.iObj[i];
				const TBool ok = aCpu.AddWindow((uint32_t)c.Base(), (uint32_t)c.MaxSize());
				RDebug::Printf("PH: proteus kernel: adopted chunk handle %x: window %x + %x %s", aHandle, (TUint)c.Base(), c.MaxSize(), ok ? "" : "(no window slot!)");
				}
			return i;
			}
	RDebug::Printf("PH: proteus kernel: no slot to adopt handle %x", aHandle);
	return -1;
	}

// kernel objects for the guest: P&S, OpenObject by name, and the calls on the opened chunks / mutexes / semaphores (native, blocking)
static bool BridgeKernel(void* aCtx, const char* aExec, uint32_t* r, proteus::CArmCpu& aCpu)
	{
	TBridge& b = *(TBridge*)aCtx;
	const TPtrC8 n((const TUint8*)aExec);
	// the process environment: this process IS the guest's (the loader created it for the program), so its parameter slots -- apparc's
	// command line in slot 1, ... -- are the guest's; the native calls are the same executive calls (guest memory is this process's)
	// the hardware abstraction layer (HAL::Get / UserHal -> UserSvr::HalFunction): the same executive call on the native kernel, whose answers are
	// the device's (the guest's pointers are this process's memory). An unanswered group used to end in KErrNotSupported, which Let's Golf 2 turned
	// into its "Feature is not supported" note (the display / digitiser / ... attributes it reads at start-up)
	if (n == _L8("EExecHalFunction"))
		{
		const uint32_t grp = r[0], fn = r[1], a1 = r[2];
		const TInt e = UserSvr::HalFunction((TInt)r[0], (TInt)r[1], (TAny*)r[2], (TAny*)r[3]);
		static TInt halLines = 0;
		if (grp != 0 && halLines < 80)		// the device attributes the guest reads (display size, mode, ...): group, function, result, first word of the answer
			{
			++halLines;
			RDebug::Printf("PH: proteus HAL: group %d fn %d -> %d (a1 word %d)", grp, fn, e, e == KErrNone && a1 > 0x1000 ? *(const TInt*)a1 : 0);
			}
		r[0] = (uint32_t)e;
		return true;
		}
	if (n == _L8("EExecProcessDataParameterLength")) { r[0] = (uint32_t)User::ParameterLength((TInt)r[0]); return true; }
	if (n == _L8("EExecProcessGetDataParameter"))		// (slot, TUint8* data, max length) -> length or an error
		{
		TPtr8 d((TUint8*)r[1], 0, (TInt)r[2]);
		// the exec returns the LENGTH read (euser GetDesParameter sets the descriptor length from it, us_exec.cpp), an error when negative;
		// returning KErrNone made every command line arrive empty (apparc then ended the app with KErrEof, -25)
		const TInt e = User::GetDesParameter((TInt)r[0], d);
		r[0] = (uint32_t)(e < 0 ? e : d.Length());
		return true;
		}
	if (n == _L8("EExecProcessCommandLineLength")) { r[0] = (uint32_t)gGuestCmd.Length(); return true; }		// characters
	if (n == _L8("EExecProcessCommandLine"))		// (process, TDes8&): the UTF-16 command line as bytes (User::CommandLine halves the length)
		{
		TDes8& d = *(TDes8*)r[1];
		d.Copy(TPtrC8((const TUint8*)gGuestCmd.Ptr(), Min(gGuestCmd.Size(), d.MaxLength())));
		return true;
		}
	// user activity: the inactivity time is the system's (the lights, screen saver and auto-lock all read it), so a game that calls
	// User::ResetInactivityTime from its frame loop keeps the display on exactly as it does on the device
	if (n == _L8("EExecResetInactivityTime")) { User::ResetInactivityTime(); return true; }
	if (n == _L8("EExecUserInactivityTime")) { r[0] = (uint32_t)User::InactivityTime().Int(); return true; }
	if (n.Left(13) == _L8("EExecProperty"))
		return BridgeProperty(aCtx, aExec, r);
	if (n == _L8("EFastExecGetGlobalUserData"))
		{
		// the kernel's GlobalUserData[index] (sexec.cpp: locale char-set data pointers, set when the locale DLL loads). No euser API returns
		// it, so the fast exec is made directly through the x64 gate (port/x64user/u32exec.h: int 0x20, the plain fast-exec number in EAX, argument in ECX).
		// The pointers lead into the (XIP) locale DLL = inside the ROM window.
		TInt v;
		__asm__ __volatile__(".byte 0xCD, 0x20" : "=a"(v) : "a"(22), "c"((TInt)r[0]) : "memory");
		r[0] = (uint32_t)v;
		return true;
		}
	if (n == _L8("EFastExecRomRootDirectoryAddress"))		// fast exec 18, no argument: the native ROM's root directory (inside the ROM window)
		{
		TInt v;
		__asm__ __volatile__(".byte 0xCD, 0x20" : "=a"(v) : "a"(18) : "memory");
		r[0] = (uint32_t)v;
		return true;
		}
	// time: the native clock (guest pointers = this process's memory)
	if (n == _L8("EExecTimeNow") || n == _L8("EExecTimeNowSecure"))		// (TInt64& utc, TInt& offset seconds)
		{
		TTime t;
		t.UniversalTime();
		Mem::Copy((TAny*)r[0], &t.Int64(), sizeof(TInt64));
		*(TInt*)r[1] = User::UTCOffset().Int();
		r[0] = KErrNone;
		return true;
		}
	if (n == _L8("EFastExecUTCOffset")) { r[0] = (uint32_t)User::UTCOffset().Int(); return true; }
	if (n == _L8("EFastExecNTickCount")) { r[0] = User::NTickCount(); return true; }
	if (n == _L8("EExecTickCount")) { r[0] = User::TickCount(); return true; }
	if (n == _L8("EExecHandleCount"))		// (thread handle, TInt& process handles, TInt& thread handles): this process's, where the guest's live
		{
		TInt ph = 0, th = 0;
		RThread().HandleCount(ph, th);
		aCpu.Write32(r[1], (TUint32)ph);
		aCpu.Write32(r[2], (TUint32)th);
		return true;
		}
	if (n == _L8("EFastExecFastCounter")) { r[0] = User::FastCounter(); return true; }
	if (n == _L8("EExecTimerCreate"))		// a native RTimer; type 6 = ETimer
		{
		TInt slot = -1;
		for (TInt i = 0; i < KMaxBridge && slot < 0; ++i) if (!b.iObjType[i]) slot = i;
		if (slot < 0) { r[0] = (uint32_t)KErrNoMemory; return true; }
		const TInt e = reinterpret_cast<RTimer&>(b.iObj[slot]).CreateLocal();
		if (e != KErrNone) { r[0] = (uint32_t)e; return true; }
		b.iObjType[slot] = 6;
		r[0] = KObjHandle0 + slot;
		return true;
		}
	if (n == _L8("EExecMsgQueueCreate"))		// (const TDesC* name or NULL, slots, message length, owner type): a native RMsgQueue; type 14 = EMsgQueue
		{
		TInt slot = -1;
		for (TInt i = 0; i < KMaxBridge && slot < 0; ++i) if (!b.iObjType[i]) slot = i;
		if (slot < 0) { r[0] = (uint32_t)KErrNoMemory; return true; }
		if ((TInt)r[1] <= 0 || (TInt)r[2] < 4 || (r[2] & 3) || r[2] > 256) { r[0] = (uint32_t)KErrArgument; return true; }		// (the kernel panics: KERN-EXEC 49 / 48)
		RMsgQueueBase& q = reinterpret_cast<RMsgQueueBase&>(b.iObj[slot]);
		const TOwnerType owner = (TOwnerType)r[3];
		const TInt e = r[0] ? q.CreateGlobal(*(const TDesC*)r[0], (TInt)r[1], (TInt)r[2], owner) : q.CreateLocal((TInt)r[1], (TInt)r[2], owner);
		if (e != KErrNone) { r[0] = (uint32_t)e; return true; }
		b.iObjType[slot] = 14;
		r[0] = KObjHandle0 + slot;
		return true;
		}
	if (n == _L8("EExecChangeNotifierCreate"))		// a native RChangeNotifier (locale / midnight / ... changes); type 12 = EChangeNotifier
		{
		TInt slot = -1;
		for (TInt i = 0; i < KMaxBridge && slot < 0; ++i) if (!b.iObjType[i]) slot = i;
		if (slot < 0) { r[0] = (uint32_t)KErrNoMemory; return true; }
		const TInt e = reinterpret_cast<RChangeNotifier&>(b.iObj[slot]).Create();
		if (e != KErrNone) { r[0] = (uint32_t)e; return true; }
		b.iObjType[slot] = 12;
		r[0] = KObjHandle0 + slot;
		return true;
		}
	if (n == _L8("EExecPlatSecDiagnostic"))	// (TPlatSecDiagnostic*): a guest-side check failed -- log it, answer as the kernel (ssecure.cpp)
		{
		const TInt32* d = (const TInt32*)r[0];		// iType, iArg1, iArg2, iContextText, iContextTextLength (u32std.h)
		TBuf8<80> ctx;
		if (d && d[3] && d[4] > 0)
			ctx.Copy(TPtrC8((const TUint8*)d[3], Min(d[4], ctx.MaxLength())));
		RDebug::Printf("PH: proteus PlatSec diagnostic: type %d arg1 %08x arg2 %08x '%S'", d ? d[0] : -1, d ? d[1] : 0, d ? d[2] : 0, &ctx);
		r[0] = (uint32_t)(PlatSec::ConfigSetting(PlatSec::EPlatSecEnforcement) ? KErrPermissionDenied : KErrNone);
		return true;
		}
	if (n == _L8("EExecObjectNext"))		// (TObjectType, TBuf8<KMaxFullName>& match -> full name, TFindHandle& cursor): TFindXxx::Next on the native kernel
		{
		TDes8& name8 = *(TDes8*)r[1];		// guest descriptor = native layout, same memory
		TFullName match, found;
		match.Copy(name8.Left(match.MaxLength()));
		TFindHandle& cursor = *(TFindHandle*)r[2];
		TInt e = KErrNotSupported;
		switch ((TInt)r[0])		// TObjectType (u32std.h)
			{
			case 0: e = FindNext<TFindThread>(match, cursor, found); break;
			case 1: e = FindNext<TFindProcess>(match, cursor, found); break;
			case 2: e = FindNext<TFindChunk>(match, cursor, found); break;
			case 3: e = FindNext<TFindLibrary>(match, cursor, found); break;
			case 4: e = FindNext<TFindSemaphore>(match, cursor, found); break;
			case 5: e = FindNext<TFindMutex>(match, cursor, found); break;
			case 7: e = FindNext<TFindServer>(match, cursor, found); break;
			case 9: e = FindNext<TFindLogicalDevice>(match, cursor, found); break;
			case 10: e = FindNext<TFindPhysicalDevice>(match, cursor, found); break;
			}
		if (e == KErrNone) name8.Copy(found.Left(name8.MaxLength()));
		r[0] = (uint32_t)e;
		return true;
		}
	if (n == _L8("EExecOpenObject"))		// (TObjectType, const TDesC8& name, TOwnerType)
		{
		const TInt type = (TInt)r[0];
		const TDesC8& name8 = *(const TDesC8*)r[1];		// guest descriptor = native layout, same memory
		TFullName name;
		name.Copy(name8.Left(name.MaxLength()));
		TInt slot = -1;
		for (TInt i = 0; i < KMaxBridge && slot < 0; ++i) if (!b.iObjType[i]) slot = i;
		if (slot < 0) { r[0] = (uint32_t)KErrNoMemory; return true; }
		RHandleBase& h = b.iObj[slot];
		TInt e = KErrNotSupported;
		if (type == 2) e = b.iObj[slot].OpenGlobal(name, EFalse);
		else if (type == 5) e = reinterpret_cast<RMutex&>(h).OpenGlobal(name);
		else if (type == 4) e = reinterpret_cast<RSemaphore&>(h).OpenGlobal(name);
		TBuf8<80> nm;
		nm.Copy(name.Left(70));
		RDebug::Printf("PH: proteus kernel: OpenObject type %d '%S' -> %d", type, &nm, e);
		if (e != KErrNone) { r[0] = (uint32_t)e; return true; }
		b.iObjType[slot] = type;
		if (type == 2)		// TObjectType (u32std.h): 2 EChunk, 4 ESemaphore, 5 EMutex
			{
			RChunk& c = b.iObj[slot];
			const TBool ok = aCpu.AddWindow((uint32_t)c.Base(), (uint32_t)c.MaxSize());
			RDebug::Printf("PH: proteus kernel: chunk window %x + %x %s", (TUint)c.Base(), c.MaxSize(), ok ? "" : "(no window slot!)");
			}
		r[0] = KObjHandle0 + slot;
		return true;
		}
	if ((n == _L8("EExecChunkBase") || n == _L8("EExecChunkSize") || n == _L8("EExecChunkMaxSize") || n == _L8("EExecChunkBottom") || n == _L8("EExecChunkTop"))
		&& ObjIndex(b, r[0]) < 0 && NativeLooking(r[0]))		// a chunk a server's reply handed over
		AdoptObj(b, aCpu, r[0], 2);
	if (n.Left(13) == _L8("EExecMsgQueue") && n != _L8("EExecMsgQueueCreate") && ObjIndex(b, r[0]) < 0 && NativeLooking(r[0]))
		AdoptObj(b, aCpu, r[0], 14);		// a message queue a server's reply handed over (RDirectScreenAccess: the window server's two DSA queues)
	const TInt i = ObjIndex(b, r[0]);
	if (i < 0)
		return false;
	RChunk& c = b.iObj[i];
	if (n == _L8("EExecHandleClose")) { c.Close(); b.iObjType[i] = 0; b.iObjNative[i] = 0; r[0] = KErrNone; return true; }
	// timers: the guest's TRequestStatus is this process's memory; the native kernel completes it and signals this thread
	RTimer& tm = reinterpret_cast<RTimer&>(c);
	if (n == _L8("EExecTimerAfter")) { tm.After(*(TRequestStatus*)r[1], TTimeIntervalMicroSeconds32((TInt)r[2])); return true; }
	if (n == _L8("EExecTimerHighRes")) { tm.HighRes(*(TRequestStatus*)r[1], TTimeIntervalMicroSeconds32((TInt)r[2])); return true; }
	if (n == _L8("EExecTimerAgainHighRes")) { tm.AgainHighRes(*(TRequestStatus*)r[1], TTimeIntervalMicroSeconds32((TInt)r[2])); return true; }
	if (n == _L8("EExecTimerInactivity")) { tm.Inactivity(*(TRequestStatus*)r[1], TTimeIntervalSeconds((TInt)r[2])); return true; }
	if (n == _L8("EExecTimerLock")) { tm.Lock(*(TRequestStatus*)r[1], (TTimerLockSpec)r[2]); return true; }
	if (n == _L8("EExecTimerAt"))		// (handle, status, TTime as lo, hi) -- the exec takes kernel time (UTC); RTimer::AtUTC is the same call
		{
		const TInt64 t = MAKE_TINT64(r[3], r[2]);
		tm.AtUTC(*(TRequestStatus*)r[1], TTime(t));
		return true;
		}
	if (n == _L8("EExecTimerCancel")) { SigTrace("timer-cancel", 0, 0); tm.Cancel(); SigTrace("timer-cancelled", 0, 0); return true; }
	if (n == _L8("EExecChangeNotifierLogon")) { r[0] = (uint32_t)reinterpret_cast<RChangeNotifier&>(c).Logon(*(TRequestStatus*)r[1]); return true; }
	if (n == _L8("EExecChangeNotifierLogoff")) { SigTrace("notifier-logoff", 0, 0); r[0] = (uint32_t)reinterpret_cast<RChangeNotifier&>(c).LogonCancel(); return true; }
	if (n == _L8("EExecChunkBase")) { r[0] = (uint32_t)c.Base(); return true; }
	if (n == _L8("EExecChunkSize")) { r[0] = (uint32_t)c.Size(); return true; }
	if (n == _L8("EExecChunkMaxSize")) { r[0] = (uint32_t)c.MaxSize(); return true; }
	if (n == _L8("EExecChunkBottom")) { r[0] = (uint32_t)c.Bottom(); return true; }
	if (n == _L8("EExecChunkTop")) { r[0] = (uint32_t)c.Top(); return true; }
	if (b.iObjType[i] == 14)		// a message queue: the calls are the kernel's own, on this process's queue (guest pointers are this process's memory)
		{
		RMsgQueueBase& q = reinterpret_cast<RMsgQueueBase&>(c);
		if (n == _L8("EExecMsgQueueSend")) { r[0] = (uint32_t)q.Send((const TAny*)r[1], (TInt)r[2]); return true; }
		if (n == _L8("EExecMsgQueueReceive")) { r[0] = (uint32_t)q.Receive((TAny*)r[1], (TInt)r[2]); return true; }
		if (n == _L8("EExecMsgQueueNotifySpaceAvailable")) { q.NotifySpaceAvailable(*(TRequestStatus*)r[1]); return true; }
		if (n == _L8("EExecMsgQueueNotifyDataAvailable")) { q.NotifyDataAvailable(*(TRequestStatus*)r[1]); return true; }
		if (n == _L8("EExecMsgQueueCancelSpaceAvailable")) { q.CancelSpaceAvailable(); return true; }
		if (n == _L8("EExecMsgQueueCancelDataAvailable")) { q.CancelDataAvailable(); return true; }
		if (n == _L8("EExecMsgQueueSize")) { r[0] = (uint32_t)q.MessageSize(); return true; }
		}
	if (n == _L8("EExecMutexWait")) { reinterpret_cast<RMutex&>(c).Wait(); r[0] = KErrNone; return true; }
	if (n == _L8("EExecMutexSignal")) { reinterpret_cast<RMutex&>(c).Signal(); return true; }
	if (n == _L8("EExecMutexIsHeld")) { r[0] = reinterpret_cast<RMutex&>(c).IsHeld(); return true; }
	if (n == _L8("EExecSemaphoreWait")) { reinterpret_cast<RSemaphore&>(c).Wait(); r[0] = KErrNone; return true; }
	if (n == _L8("EExecSemaphoreSignal1")) { reinterpret_cast<RSemaphore&>(c).Signal(); return true; }
	if (n == _L8("EExecSemaphoreSignalN")) { reinterpret_cast<RSemaphore&>(c).Signal((TInt)r[1]); return true; }
	RDebug::Printf("PH: proteus kernel: %s on an opened object is not bridged", aExec);
	return false;
	}

// ---- MMF controller threads, hosted natively (docs/sparkle-gl-run.md "Music") ----
// An MMF client (CMdaAudioPlayerUtility, CVideoPlayerUtility, the recorders: all through RMMFController::Open -> RMMFControllerProxy::LoadController)
// runs the controller plug-in in a thread of its OWN process behind a private server: it creates the thread (entry
// CMMFControllerProxyServer::StartThread, argument {RServer2*, shared heap}), waits for its rendezvous, then CreateSession(the server) and talks to
// the controller by IPC messages only; the first one, function 0 (EMMFControllerProxyLoadControllerPluginByUid), carries the plug-in's UID.
// The controller chain (framework, ECOM plug-in, codec, HwDevice, CAF) is the NATIVE one, so the guest's thread is hosted here: a thread
// created with its entry in the guest's mmfcontrollerframework is never run as ARM code. Its Resume gives the guest a server handle (written
// into the guest's RServer2) and the rendezvous result; the session on that handle is a bridge session whose first message loads the controller,
// natively, by RMMFControllerProxy::LoadController on a HELPER THREAD of this process: the helper owns the native proxy (its active scheduler, the
// logon monitor of the controller thread, whose completion signals belong to the helper and not to the guest's request accounting) and completes
// the guest's TRequestStatus when the load is done. From then on the guest's session is the proxy's own session (shared process-wide), and every
// message -- AddDataSource, Prime, Play, ReceiveEvents (async, completed natively into the guest's status), custom commands -- goes through the
// ordinary bridge, descriptors in guest memory as they are. Closing the session closes the proxy (the controller thread exits) and ends the
// guest's thread: its logons complete.
enum { KServerHandle0 = 0x00E50000, KMaxCtl = 16 };
enum { ECtlLoad = 1, ECtlClose = 2 };
struct TCtlHost
	{
	TBool iUsed;
	TInt iThread;				// the guest thread (the executive's thread number)
	TUint32 iParams;			// the guest's TControllerProxyServerParams
	TBool iShared;
	TBool iStarted;
	RThread iHelper;			// the helper thread
	TRequestStatus* volatile iCmdSt;	// its command active object's status (pending)
	volatile TInt iCmd;
	TInt32 iUid;				// the controller plug-in (command ECtlLoad)
	TRequestStatus* iGuestSt;	// the guest's status the load completes
	RMMFControllerProxy* iProxy;	// owned by the helper
	volatile TBool iLoaded;		// the load succeeded: iProxy's session is the guest's
	TBool iBound;				// the bridge session wraps iProxy's handle
	RThread iCtl;				// the controller thread (found after the load)
	TThreadId iCtlId;
	TBool iHaveCtl;
	RSemaphore iDone;			// signalled by the helper when it has closed the proxy
	volatile TBool iDead;
	TBool iSessionOpen;			// the guest holds a session (CtlShutdown clears it)
	TBool iReported;				// the executive has been told the thread ended (iHostExit)
	TInt iExitType, iExitReason;
	};
static TCtlHost gCtl[KMaxCtl];
static RThread gMainThread;		// the handler's main thread (where the guest runs): the helper completes the guest's statuses on it
static RMutex gCtlLock;			// one controller load at a time: the controller thread is found by what is new
static TBool gCtlInit = EFalse;

static TInt CtlIndexOfServer(uint32_t aHandle) { const TInt i = (TInt)(aHandle - KServerHandle0); return (aHandle & 0xFFFF0000u) == KServerHandle0 && i >= 0 && i < KMaxCtl && gCtl[i].iUsed ? i : -1; }

class CCtlCmd : public CActive
	{
public:
	explicit CCtlCmd(TCtlHost& aHost) : CActive(EPriorityStandard), iHost(aHost) { CActiveScheduler::Add(this); }
	~CCtlCmd() { Cancel(); }
	void Arm() { iStatus = KRequestPending; iHost.iCmdSt = &iStatus; SetActive(); }
	void RunL();
	void DoCancel() { TRequestStatus* s = &iStatus; User::RequestComplete(s, KErrCancel); }
private:
	void FindController();
	TCtlHost& iHost;
	};

// The controller thread just created by this helper's LoadController (called under gCtlLock): a live thread of THIS process named
// "MMFControllerProxyServer-<its id in hex>" (RMMFControllerProxy's CMMFControllerProxyServer::RenameControllerProxyThread) that no other hosted
// controller has claimed. (The thread is found by its name -- the find handle itself cannot be opened, the thread being owned by the helper --
// and opened by the id the name carries.)
void CCtlCmd::FindController()
	{
	TFullName self(RThread().FullName());
	const TInt colons = self.Find(_L("::"));
	TFindThread find(_L("*::MMFControllerProxyServer-*"));
	TFullName name;
	while (!iHost.iHaveCtl && find.Next(name) == KErrNone)
		{
		if (colons < 0 || name.Left(colons + 2).Compare(self.Left(colons + 2)) != 0)
			continue;		// another process's
		const TInt dash = name.LocateReverse('-');
		TUint32 id = 0;
		TLex lex(name.Mid(dash + 1));
		if (dash < 0 || lex.Val(id, EHex) != KErrNone)
			continue;
		TBool taken = EFalse;
		for (TInt k = 0; k < KMaxCtl && !taken; ++k)
			if (gCtl[k].iUsed && gCtl[k].iHaveCtl && gCtl[k].iCtlId == TThreadId(id)) taken = ETrue;
		RThread t;
		if (taken || t.Open(TThreadId(id)) != KErrNone)
			continue;
		if (t.ExitType() != EExitPending)
			{
			t.Close();
			continue;
			}
		iHost.iCtl = t;
		iHost.iCtlId = TThreadId(id);
		iHost.iHaveCtl = ETrue;
		}
	}

void CCtlCmd::RunL()
	{
	if (iHost.iCmd == ECtlLoad)
		{
		TInt r = KErrNoMemory;
		iHost.iProxy = new RMMFControllerProxy;
		if (iHost.iProxy)
			{
			gCtlLock.Wait();
			r = iHost.iProxy->LoadController(TUid::Uid(iHost.iUid), iHost.iShared);
			if (r == KErrNone)
				r = iHost.iProxy->ShareAuto();		// the guest's threads use the session (the controller proxy server is globally sharable)
			if (r == KErrNone)
				FindController();
			gCtlLock.Signal();
			TLog8 cn(iHost.iHaveCtl ? (const TDesC&)iHost.iCtl.Name() : (const TDesC&)KNullDesC());
			RDebug::Printf("PH: proteus MMF controller %x: native LoadController -> %d (controller thread %S)", iHost.iUid, r, cn.P());
			if (r != KErrNone)
				{
				iHost.iProxy->Close();
				delete iHost.iProxy;
				iHost.iProxy = NULL;
				}
			}
		iHost.iLoaded = r == KErrNone;
		TRequestStatus* g = iHost.iGuestSt;
		iHost.iGuestSt = NULL;
		Arm();
		gMainThread.RequestComplete(g, r);		// (the guest's status; the kernel signals the main thread, which credits the request)
		}
	else
		{
		if (iHost.iProxy)
			{
			iHost.iProxy->Close();		// closes the session: the controller thread shuts down and Close waits for it
			delete iHost.iProxy;
			iHost.iProxy = NULL;
			}
		iHost.iLoaded = EFalse;
		iHost.iDone.Signal();
		CActiveScheduler::Stop();
		}
	}

static TInt CtlHelperMain(TAny* aHost)
	{
	TCtlHost& h = *(TCtlHost*)aHost;
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TInt r = KErrNoMemory;
	if (cleanup)
		{
		CActiveScheduler* sched = new CActiveScheduler;
		if (sched)
			{
			CActiveScheduler::Install(sched);
			CCtlCmd* cmd = new CCtlCmd(h);
			if (cmd)
				{
				cmd->Arm();
				RThread::Rendezvous(KErrNone);
				CActiveScheduler::Start();
				delete cmd;
				r = KErrNone;
				}
			CActiveScheduler::Install(NULL);
			delete sched;
			}
		delete cleanup;
		}
	if (r != KErrNone)
		RThread::Rendezvous(r);
	return r;
	}

static void CtlInit()
	{
	if (gCtlInit)
		return;
	gCtlInit = ETrue;
	gMainThread.Open(RThread().Id());
	gCtlLock.CreateLocal();
	for (TInt i = 0; i < KMaxCtl; ++i)
		gCtl[i].iHelper.SetHandle(KNullHandle);
	}

// TIpcBridge::iHostThread: the guest created a thread whose entry is in mmfcontrollerframework -- an MMF controller thread
static bool CtlHostThread(void*, const char* aModule, int aThread, uint32_t aArg)
	{
	TBuf8<48> m;
	m.Copy(TPtrC8((const TUint8*)aModule).Left(40));
	m.LowerCase();
	if (m.Find(_L8("mmfcontrollerframework")) < 0)
		return false;
	CtlInit();
	for (TInt i = 0; i < KMaxCtl; ++i)
		if (!gCtl[i].iUsed)
			{
			TCtlHost& c = gCtl[i];
			c.iUsed = ETrue;
			c.iThread = aThread;
			c.iParams = aArg;
			c.iShared = aArg ? ((TUint32*)aArg)[1] != 0 : EFalse;		// TControllerProxyServerParams {RServer2* iServer; TBool iUsingSharedHeap}
			c.iStarted = c.iLoaded = c.iBound = c.iHaveCtl = c.iDead = c.iSessionOpen = c.iReported = EFalse;
			c.iProxy = NULL;
			c.iGuestSt = NULL;
			c.iCmdSt = NULL;
			c.iExitType = c.iExitReason = 0;
			RDebug::Printf("PH: proteus MMF controller thread %d: hosted natively (shared heap %d)", i, c.iShared);
			return true;
			}
	RDebug::Printf("PH: proteus MMF controller thread: no free slot (%d hosted)", (TInt)KMaxCtl);
	return false;
	}


// the guest's first Resume of the thread: the helper starts (the native thread that will own the proxy), and the guest's RServer2 gets its handle
static int CtlHostStart(void*, int aThread)
	{
	TInt i = -1;
	for (TInt k = 0; k < KMaxCtl; ++k)
		if (gCtl[k].iUsed && gCtl[k].iThread == aThread) i = k;
	if (i < 0)
		return KErrNotFound;
	TCtlHost& c = gCtl[i];
	TInt r = c.iDone.CreateLocal(0);
	if (r == KErrNone)
		r = c.iHelper.Create(KNullDesC, CtlHelperMain, 0x14000, 0x8000, 0x1000000, &c, EOwnerProcess);
	if (r == KErrNone)
		{
		TRequestStatus rs;
		c.iHelper.Rendezvous(rs);
		c.iHelper.Resume();
		User::WaitForRequest(rs);
		r = rs.Int();
		}
	if (r != KErrNone)
		{
		RDebug::Printf("PH: proteus MMF controller thread %d: helper start -> %d", i, r);
		return r;
		}
	c.iStarted = ETrue;
	if (c.iParams && ((TUint32*)c.iParams)[0])
		*(TUint32*)((TUint32*)c.iParams)[0] = KServerHandle0 + i;		// the guest's RServer2 (its iHandle is its only member)
	RDebug::Printf("PH: proteus MMF controller thread %d: started, server handle %x", i, (TUint)(KServerHandle0 + i));
	return KErrNone;
	}

// the slot is free again once the executive has been told the thread ended and the guest has closed its session
static void CtlMaybeFree(TCtlHost& c) { if (c.iReported && !c.iSessionOpen) c.iUsed = EFalse; }

static bool CtlHostExit(void*, int aThread, int* aType, int* aReason)
	{
	for (TInt k = 0; k < KMaxCtl; ++k)
		if (gCtl[k].iUsed && gCtl[k].iThread == aThread)
			{
			TCtlHost& c = gCtl[k];
			if (!c.iDead && c.iHaveCtl && c.iCtl.ExitType() != EExitPending)		// the controller thread ended by itself (a panic, a kill)
				{
				c.iDead = ETrue;
				c.iExitType = c.iCtl.ExitType();
				c.iExitReason = c.iCtl.ExitReason();
				RDebug::Printf("PH: proteus MMF controller thread %d: ended type %d reason %d", k, c.iExitType, c.iExitReason);
				}
			if (!c.iDead)
				return false;
			*aType = c.iExitType;
			*aReason = c.iExitReason;
			c.iReported = ETrue;
			CtlMaybeFree(c);
			return true;
			}
	return false;
	}

// the guest's RThread::Kill of the controller thread (Close's last resort when it does not exit in time): the controller thread, natively
static void CtlHostKill(void*, int aThread, int aReason)
	{
	for (TInt k = 0; k < KMaxCtl; ++k)
		if (gCtl[k].iUsed && gCtl[k].iThread == aThread && gCtl[k].iHaveCtl && gCtl[k].iCtl.ExitType() == EExitPending)
			{
			RDebug::Printf("PH: proteus MMF controller thread %d: killed by the guest (%d)", k, aReason);
			gCtl[k].iCtl.Kill(aReason);
			}
	}

// SessionCreateFromHandle on a server handle CtlHostStart handed out: a bridge session that is the controller's, bound to the native proxy's
// session when the load has completed
static int CtlSessionFromServer(void* aCtx, uint32_t aServerHandle, int aSlots)
	{
	TBridge& b = *(TBridge*)aCtx;
	const TInt c = CtlIndexOfServer(aServerHandle);
	if (c < 0)
		return KErrBadHandle;
	for (TInt i = 0; i < KMaxBridge; ++i)
		if (!b.iUsed[i])
			{
			b.iUsed[i] = ETrue;
			b.iConnected[i] = EFalse;
			b.iNative[i] = EFalse;
			b.iCtl[i] = c;
			gCtl[c].iSessionOpen = ETrue;
			b.iName[i] = _L("MMFControllerProxy");
			b.iSlots[i] = aSlots;
			RDebug::Printf("PH: proteus IPC: guest session %d -> MMF controller %d (%d slots)", i, c, aSlots);
			return KBridgeHandle0 + i;
			}
	return KErrNoMemory;
	}

// the native proxy's session, once the load has completed, is this bridge session's
static void CtlBind(TBridge& b, TInt i)
	{
	TCtlHost& c = gCtl[b.iCtl[i]];
	if (!c.iBound && c.iLoaded && c.iProxy)
		{
		b.iSession[i].SetHandle(c.iProxy->Handle());
		c.iBound = ETrue;
		}
	}

// the guest closed the controller's session: the helper closes the native proxy (the controller thread shuts down, Close waits for it) and ends;
// the guest's thread is dead (its logons complete when the executive next looks: TIpcBridge::iHostExit)
static void CtlShutdown(TBridge& b, TInt i)
	{
	TCtlHost& c = gCtl[b.iCtl[i]];
	if (c.iBound)
		b.iSession[i].SetHandle(0);		// the proxy's, not ours to close
	if (c.iStarted && c.iHelper.Handle())
		{
		c.iCmd = ECtlClose;
		TRequestStatus* s = c.iCmdSt;
		c.iHelper.RequestComplete(s, KErrNone);
		c.iDone.Wait();
		TRequestStatus ls;
		c.iHelper.Logon(ls);
		User::WaitForRequest(ls);
		c.iHelper.Close();
		c.iDone.Close();
		}
	if (!c.iDead)
		{
		c.iDead = ETrue;
		c.iExitType = c.iHaveCtl ? (TInt)c.iCtl.ExitType() : 0;
		c.iExitReason = c.iHaveCtl ? c.iCtl.ExitReason() : 0;
		}
	if (c.iHaveCtl)
		c.iCtl.Close();
	RDebug::Printf("PH: proteus MMF controller %d: closed (controller thread type %d reason %d)", b.iCtl[i], c.iExitType, c.iExitReason);
	c.iStarted = c.iLoaded = c.iBound = c.iHaveCtl = EFalse;
	c.iSessionOpen = EFalse;
	b.iCtl[i] = -1;
	CtlMaybeFree(c);
	}

// (a handle outside the guest's own ranges is a kernel handle the guest was given: a server's reply handed it a session)
static bool BridgeOwns(void* aCtx, uint32_t aHandle) { return BridgeIndex(*(TBridge*)aCtx, aHandle) >= 0 || PropIndex(*(TBridge*)aCtx, aHandle) >= 0 || CtlIndexOfServer(aHandle) >= 0 || NativeLooking(aHandle); }

static int BridgeCreate(void* aCtx, const char* aName, int aSlots)
	{
	TBridge& b = *(TBridge*)aCtx;
	for (TInt i = 0; i < KMaxBridge; ++i)
		if (!b.iUsed[i])
			{
			b.iUsed[i] = ETrue;
			b.iConnected[i] = EFalse;
			b.iName[i].Copy(TPtrC8((const TUint8*)aName));
			b.iSlots[i] = aSlots;
			RDebug::Printf("PH: proteus IPC: guest session %d -> server %s (%d slots)", i, aName, aSlots);
			if (strstr(aName, "MMF")) proteus::gExecTraceWindow = 150;
			return KBridgeHandle0 + i;
			}
	return KErrNoMemory;
	}

static void WatchAsync(TInt aFn, TRequestStatus* aSt, const TDesC& aServer);
static void UnwatchAsync(TRequestStatus* aSt);
// trace of every source of a native request signal (the next stray-signal investigation): bounded
static void SigTrace(const char* aWhat, TUint aStatus, TInt aValue)
	{
	static TInt lines = 0;
	if (lines < 4000)
		{
		++lines;
		RDebug::Printf("PH: proteus sig: %s st %x v %d count %d", aWhat, aStatus, aValue, RThread().RequestCount());
		}
	}
// set when CompleteGuest completed a guest status inside the last send: the embedder's executive credits the signal to the sending guest thread
// itself (TIpcBridge::iSyncDone), exactly -- a native RequestSignal here would be an anonymous signal, to be matched with its status later, by which time the
// guest may have reused the status for its next call
static TBool gGuestCompleted = EFalse;
static TBool BridgeSyncDone(void*)
	{
	const TBool done = gGuestCompleted;
	gGuestCompleted = EFalse;
	return done;
	}
// a synchronous guest send (SessionSendSync, e.g. sessions with 0 async slots) is done synchronously here -- an async native send would
// need a slot (KErrServerBusy) -- and the guest's TRequestStatus is completed + this thread signalled, as the kernel would
static void CompleteGuest(TRequestStatus* aStatus, TInt aResult)
	{
	if (!aStatus)
		return;
	*aStatus = aResult;
	SigTrace("complete-guest", (TUint)aStatus, aResult);
	gGuestCompleted = ETrue;
	}

// the guest's private path on a file-server session (TFsMessage, f32plugin.h). The file server derives it from the client's SecureId,
// which is this handler's: a new session starts in \private\<handler>\ (CServerFs::NewSessionL) and SessionToPrivate / PrivatePath /
// CreatePrivatePath (sf_nbs.cpp) answer the handler's directory. These give the guest its own, \private\<its SecureId>\. Started by
// the loader (core sf_lepoc.cpp ForeignCpuRedirect) the handler already has the program's SecureId and these are the server's own
// answers; started directly (the tests: "-r <exe>") it has its own, with AllFiles to reach the guest's. true = handled here.
static TBool BridgeFsPrivate(TBridge& b, TInt i, TInt aFunction, const TIpcArgs& aArgs, TInt& aResult)
	{
	enum { EFsSetSessionPath = 15, EFsSessionToPrivate = 85, EFsPrivatePath = 86, EFsCreatePrivatePath = 87 };
	TBuf<20> priv(_L("\\private\\"));
	priv.AppendNumFixedWidth(b.iSid, EHex, 8);
	priv.Append('\\');
	TFileName p;
	if (!b.iPathSet[i])			// before the session's first request: where a session of the guest's own would start
		{
		b.iPathSet[i] = ETrue;
		p.Append((TChar)RFs::GetSystemDriveChar());
		p.Append(':');
		p.Append(priv);
		b.iSession[i].Sync(EFsSetSessionPath, TIpcArgs(&p));
		}
	if (aFunction != EFsSessionToPrivate && aFunction != EFsPrivatePath && aFunction != EFsCreatePrivatePath)
		return EFalse;
	if (aFunction == EFsPrivatePath)
		{
		((TDes*)aArgs.iArgs[0])->Copy(priv);		// the guest's TDes16, in this process (same layout)
		aResult = KErrNone;
		return ETrue;
		}
	TChar ch;
	aResult = RFs::DriveToChar(aArgs.iArgs[0], ch);
	if (aResult != KErrNone)
		return ETrue;
	p.SetLength(0);
	p.Append(ch);
	p.Append(':');
	p.Append(priv);
	if (aFunction == EFsSessionToPrivate)
		aResult = b.iSession[i].Sync(EFsSetSessionPath, TIpcArgs(&p));
	else
		{
		aResult = b.iFs->MkDirAll(p);
		if (aResult == KErrAlreadyExists)
			aResult = KErrNone;		// as the server: an existing private path is success
		}
	return ETrue;
	}

static int BridgeSend(void* aCtx, uint32_t aHandle, int aFunction, uint32_t aArgs, uint32_t aStatus, bool aSync)
	{
	TBridge& b = *(TBridge*)aCtx;
	TInt i = BridgeIndex(b, aHandle);
	if (i < 0 && NativeLooking(aHandle))
		i = AdoptNative(b, aHandle);
	if (i < 0)
		return KErrBadHandle;
	TRequestStatus* st = (TRequestStatus*)aStatus;
	TIpcArgs args;
	if (aArgs)
		Mem::Copy(&args, (const TAny*)aArgs, sizeof(TIpcArgs));		// iArgs[4] + iFlags: the guest's own descriptors, in this process
	// a handle argument (TIpcArgs::EHandle) is resolved by the kernel in the sender's handle table: a guest's handle to a kernel object this
	// handler holds for it (a message queue, a chunk opened by name) means the object's real handle here
	for (TInt n = 0; aArgs && n < 4; ++n)
		if (((args.iFlags >> (3 * n)) & 7) == TIpcArgs::EHandle)
			{
			const TInt oi = ObjIndex(b, args.iArgs[n]);
			if (oi >= 0) args.iArgs[n] = b.iObj[oi].Handle();
			}
	// window-server trace (the first 120 messages): function, and for a command buffer (a TDesC8 in arg 0) its size and first opcodes
	// (each command: TUint16 opcode, TInt16 length, [TInt32 handle if opcode bit 15], data)
	static TInt wsLines = 0;
	if (b.iName[i] == _L("!Windowserver") && wsLines < 3000 && b.iConnected[i])
		{
		// every command: opcode (bit 15 = a new destination handle follows), its handle (sticky), skipping the data (padded to 4)
		TBuf8<240> ops;
		TInt len = 0;
		TBool redraw = EFalse;
		if (aArgs && (args.iFlags & 7) >= 4)		// arg 0 is a descriptor
			{
			const TDesC8& d = *(const TDesC8*)args.iArgs[0];
			len = d.Length();
			const TUint8* p = d.Ptr();
			static TUint32 dest = 0;
			for (TInt o = 0; o + 4 <= len && ops.Length() < 220; )
				{
				const TUint16 op = (TUint16)(p[o] | (p[o + 1] << 8));
				const TInt16 cl = (TInt16)(p[o + 2] | (p[o + 3] << 8));
				o += 4;
				if ((op & 0x8000) && o + 4 <= len) { dest = p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24); o += 4; }
				const TUint16 code = op & 0x7FFF;
				if (code >= 0x0f && code <= 0x13) redraw = ETrue;		// window Invalidate / BeginRedraw / EndRedraw (or a GC op of that number)
				if (code == 0x0b) redraw = ETrue;						// client GetRedraw
				ops.AppendFormat(_L8(" %x@%x"), code, dest);
				o += (cl + 3) & ~3;
				}
			}
		if (wsLines < 120 || (aFunction >= 0x10000 && wsLines < 400) || redraw)
			{
			++wsLines;
			RDebug::Printf("PH: proteus WS: fn %x len %d%S", aFunction, len, &ops);
			}
		}
	// file-server trace (the first 300 messages of the guest's file sessions): the function and the names it carries (a 16-bit descriptor
	// argument), so a guest that exits on a file error can be followed to the file
	static TInt fsLines = 0;
	if (aArgs && fsLines < 300 && b.iName[i] == _L("!FileServer") && b.iConnected[i])
		{
		++fsLines;
		TBuf8<200> nm;
		for (TInt n = 0; n < 4; ++n)
			{
			const TUint f = (args.iFlags >> (3 * n)) & 7;
			if ((f & 4) && (f & 1) && args.iArgs[n])		// a 16-bit descriptor
				{
				const TDesC16* d = (const TDesC16*)args.iArgs[n];
				if (d->Length() > 0 && d->Length() < 100)
					{
					TBuf8<100> n8;
					n8.Copy(d->Left(80));
					nm.AppendFormat(_L8(" a%d='%S'"), n, &n8);
					}
				}
			}
		RDebug::Printf("PH: proteus FS: session %d fn %d flags %x args %x %x %x %x%S", i, aFunction, args.iFlags, args.iArgs[0], args.iArgs[1], args.iArgs[2], args.iArgs[3], &nm);
		}
	// CDL server trace (Avkon's layout server; bounded): every request with its arguments (flags, the four words, a 16-bit name in arg 1)
	static TInt cdlLines = 0;
	if (b.iName[i] == _L("CdlServer") && cdlLines < 60)
		{
		++cdlLines;
		// a descriptor argument: its first two words (length|type, max length or pointer) -- read only if the argument is non-null
		TUint32 d1[2] = { 0, 0 };
		if (aArgs && ((args.iFlags >> 3) & 4) && args.iArgs[1])
			Mem::Copy(d1, (const TAny*)args.iArgs[1], sizeof(d1));
		RDebug::Printf("PH: proteus CDL: fn %d %s flags %x args %x %x %x %x (arg 1 words %x %x)", aFunction, aSync ? "sync" : "async",
			args.iFlags, args.iArgs[0], args.iArgs[1], args.iArgs[2], args.iArgs[3], d1[0], d1[1]);
		}
	{
		// trace: the first messages to any server but the window server and the file server (function, argument types as TIpcArgs holds them, values)
		static TInt ipcLines = 0;
		if (aArgs && ((ipcLines < 150 && b.iName[i] != _L("!Windowserver") && b.iName[i] != _L("!FileServer")) || b.iName[i].Find(_L("MMF")) >= 0))
			{
			++ipcLines;
			TBuf8<40> srv;
			srv.Copy(b.iName[i].Left(40));
			RDebug::Printf("PH: proteus IPC: %S session %d fn %d flags %x args %x %x %x %x%s", &srv, i, aFunction, args.iFlags,
				args.iArgs[0], args.iArgs[1], args.iArgs[2], args.iArgs[3], aSync ? " sync" : st ? " async" : " blind");
			}
	}
	if (b.iCtl[i] >= 0)		// a natively hosted MMF controller (see CtlHostThread)
		{
		TCtlHost& c = gCtl[b.iCtl[i]];
		if (aFunction == RMessage2::EConnect && !b.iConnected[i])		// the version check is the native proxy's own, at its CreateSession
			{
			b.iConnected[i] = ETrue;
			CompleteGuest(st, KErrNone);
			return KErrNone;
			}
		CtlBind(b, i);
		if (!c.iBound)
			{
			// the first message loads the controller (EMMFControllerProxyLoadControllerPluginByUid: arg 0 the destination, arg 1 the plug-in's UID);
			// the helper thread does it natively and completes the guest's status
			TInt err = KErrNotReady;
			if (aFunction == 0 && st && aArgs && c.iStarted && !c.iGuestSt && !c.iLoaded && ((args.iFlags >> 3) & 7) >= 4 && args.iArgs[1])
				{
				const TDesC8& d = *(const TDesC8*)args.iArgs[1];
				if (d.Length() >= (TInt)sizeof(TInt32))
					{
					c.iUid = *(const TInt32*)d.Ptr();
					c.iGuestSt = st;
					c.iCmd = ECtlLoad;
					TRequestStatus* cs = c.iCmdSt;
					c.iHelper.RequestComplete(cs, KErrNone);
					return KErrNone;
					}
				err = KErrArgument;
				}
			RDebug::Printf("PH: proteus IPC: MMF controller %d: message %d before the load -> %d", b.iCtl[i], aFunction, err);
			CompleteGuest(st, err);
			return st ? KErrNone : err;
			}
		}
	if (aFunction == RMessage2::EConnect && !b.iConnected[i] && b.iName[i] == _L("!Windowserver") && gWsIndex < 0)
		{
		b.iConnected[i] = ETrue;
		const TInt r = BridgeWsConnect(b, i);
		RDebug::Printf("PH: proteus IPC: connect window-server session %d -> %d", i, r);
		CompleteGuest(st, r);
		return aSync || st ? KErrNone : r;
		}
	if (i == gWsIndex && aFunction == 2)		// EWservMessInit: gWs did it at the connect; the guest gets the same client handle
		{
		CompleteGuest(st, gWs.WsHandle());
		return KErrNone;
		}
	if (aFunction == RMessage2::EConnect && !b.iConnected[i])
		{
		b.iConnected[i] = ETrue;
		const TInt r = b.iSession[i].Connect(b.iName[i], args.iArgs[0], b.iSlots[i], aSync ? NULL : st);
		RDebug::Printf("PH: proteus IPC: connect session %d -> %d", i, r);
		if (aSync)
			{
			CompleteGuest(st, r);
			return KErrNone;
			}
		return r;
		}
	if (b.iName[i] == _L("!FileServer") && b.iSid)
		{
		TInt r = KErrNone;
		if (BridgeFsPrivate(b, i, aFunction, args, r))
			{
			if (r < 0) RDebug::Printf("PH: proteus IPC: !FileServer private-path fn %d -> %d", aFunction, r);
			if (aSync || st)
				{
				CompleteGuest(st, r);
				return KErrNone;
				}
			return r;
			}
		}
	if (aSync)
		{
		UnwatchAsync(st);		// (a status reused for a synchronous call)
		const TInt r = b.iSession[i].Sync(aFunction, args);
		// window-server replies to the screen-device queries (sizes, modes, rotation, origin, scale): the words the guest reads back (bounded)
		static TInt wsRep = 0;
		if (b.iName[i] == _L("!Windowserver") && wsRep < 150 && aFunction == 4 && aArgs && ((args.iFlags >> 3) & 5) == 4 && (args.iFlags & 5) == 4 && args.iArgs[1])
			{
			const TDesC8& d = *(const TDesC8*)args.iArgs[0];
			const TDesC8& rep = *(const TDesC8*)args.iArgs[1];
			if (d.Length() >= 4 && rep.Length() > 0)
				{
				++wsRep;
				const TUint8* p = d.Ptr();
				TBuf8<80> w;
				for (TInt k = 0; k + 4 <= rep.Length() && k < 24; k += 4)
					w.AppendFormat(_L8(" %x"), *(const TUint32*)(rep.Ptr() + k));
				RDebug::Printf("PH: proteus WSR: op %x len %d -> %d, reply (%d bytes)%S", (p[0] | (p[1] << 8)) & 0x7FFF, d.Length(), r, rep.Length(), &w);
				}
			}
		static TInt errLines = 0, nomemLines = 0;
		if (r < 0 && (errLines < 200 || (r == KErrNoMemory && nomemLines < 100)))		// trace: a server call the guest gets an error from (KErrNoMemory always, bounded on its own) (server, function, arg 0 if a 16-bit descriptor: a file name)
			{
			if (r == KErrNoMemory) ++nomemLines; else ++errLines;
			TBuf8<120> a0;
			if (aArgs && (args.iFlags & 7) == 7)		// TIpcArgs::EDesC16
				a0.Copy(((const TDesC16*)args.iArgs[0])->Left(110));
			else if (aArgs && (args.iFlags & 7) == 6)	// EDesC8
				a0.Copy(((const TDesC8*)args.iArgs[0])->Left(110));
			TBuf8<40> srv;
			srv.Copy(b.iName[i].Left(40));
			RDebug::Printf("PH: proteus IPC: %S fn %d -> %d %S", &srv, aFunction, r, &a0);
			}
		CompleteGuest(st, r);
		return KErrNone;
		}
	if (st)
		{
		b.iSession[i].Async(aFunction, args, *st);
		WatchAsync(aFunction, st, b.iName[i]);
		return KErrNone;
		}
	return b.iSession[i].Blind(aFunction, args);
	}

static int BridgeClose(void* aCtx, uint32_t aHandle)
	{
	TBridge& b = *(TBridge*)aCtx;
	if (CtlIndexOfServer(aHandle) >= 0)		// the guest's RServer2 of a hosted controller thread: nothing to release
		return KErrNone;
	const TInt pi = PropIndex(b, aHandle);
	if (pi >= 0)
		{
		b.iProp[pi].Close();
		b.iPropUsed[pi] = EFalse;
		return KErrNone;
		}
	TInt i = BridgeIndex(b, aHandle);
	if (i < 0 && NativeLooking(aHandle))
		i = AdoptNative(b, aHandle);
	if (i < 0)
		return KErrBadHandle;
	if (aHandle & 0x8000)		// KHandleNoClose
		return KErrNone;
	if (b.iCtl[i] >= 0)
		{
		CtlShutdown(b, i);
		b.iUsed[i] = EFalse;
		b.iConnected[i] = EFalse;
		return KErrNone;
		}
	if (b.iConnected[i] && i == gWsIndex)		// the window-server session is gWs's (BridgeWsConnect)
		{
		b.iSession[i].SetHandle(0);
		gWs.Close();
		gWsIndex = -1;
		}
	else if (b.iConnected[i])
		b.iSession[i].Close();
	b.iUsed[i] = EFalse;
	b.iNative[i] = EFalse;
	return KErrNone;
	}

static int BridgeShare(void* aCtx, uint32_t aHandle, int)
	{
	TBridge& b = *(TBridge*)aCtx;
	TInt i = BridgeIndex(b, aHandle);
	if (i < 0 && NativeLooking(aHandle))
		i = AdoptNative(b, aHandle);
	if (i >= 0 && b.iCtl[i] >= 0)
		return KErrNone;		// (the native proxy's session is shared already)
	return i >= 0 && b.iConnected[i] ? b.iSession[i].ShareAuto() : KErrNone;
	}

// window-server async requests of the guest (EventReady, RedrawReady, ...): remember the status, report when the native WSERV completes it
// (and the other servers' async requests: reported when they complete with an error, bounded)
struct TAsyncWatch { TInt iFn; TRequestStatus* iSt; TBuf8<24> iSrv; };
static TAsyncWatch gAsync[32];
static void WatchAsync(TInt aFn, TRequestStatus* aSt, const TDesC& aServer)
	{
	TInt k = 0;
	while (k < 32 && gAsync[k].iSt != aSt) ++k;		// the same request status again (reused): that entry, else a free one
	if (k == 32)
		for (k = 0; k < 32 && gAsync[k].iSt; ++k) {}
	if (k == 32)
		return;
	gAsync[k].iFn = aFn;
	gAsync[k].iSt = aSt;
	gAsync[k].iSrv.Copy(aServer.Left(24));
	}
static void UnwatchAsync(TRequestStatus* aSt)
	{
	for (TInt k = 0; k < 32; ++k)
		if (gAsync[k].iSt == aSt) gAsync[k].iSt = NULL;
	}
static void CheckAsync()
	{
	static TInt errLines = 0, nomemLines = 0;
	for (TInt k = 0; k < 32; ++k)
		if (gAsync[k].iSt && gAsync[k].iSt->Int() != KRequestPending)
			{
			const TInt v = gAsync[k].iSt->Int();
			if (gAsync[k].iSrv == _L8("!Windowserver"))
				RDebug::Printf("PH: proteus WS: async fn %x completed %d", gAsync[k].iFn, v);
			else if (v < 0 && (errLines < 100 || (v == KErrNoMemory && nomemLines < 100)))
				{
				if (v == KErrNoMemory) ++nomemLines; else ++errLines;
				RDebug::Printf("PH: proteus IPC: %S async fn %d completed %d", &gAsync[k].iSrv, gAsync[k].iFn, v);
				}
			gAsync[k].iSt = NULL;
			}
	}

static void BridgeWaitAny(void*)
	{
	CheckAsync();
	static TInt waits = 0;
	if (++waits <= 10 || (waits % 500) == 0)		// trace: the guest is idle in its active scheduler (and how often it wakes)
		{
		RDebug::Printf("PH: proteus wait %d", waits);
		// input investigation: which window group has the native WSERV's keyboard focus (the guest's app group, or another)?
		RWsSession ws;
		if (waits > 10 && ws.Connect() == KErrNone)
			{
			const TInt fg = ws.GetFocusWindowGroup();
			TBuf<80> nm;
			ws.GetWindowGroupNameFromIdentifier(fg, nm);
			TBuf8<80> n8;
			n8.Copy(nm.Left(70));
			for (TInt k = 0; k < n8.Length(); ++k) if (n8[k] < 32) n8[k] = '|';
			RDebug::Printf("PH: proteus focus: group %d '%S'", fg, &n8);
			ws.Close();
			}
		}
	User::WaitForAnyRequest();
	SigTrace("took in wait", 0, 0);
	CheckAsync();
	}
static void BridgeSignal(void*, int aCount) { SigTrace("bridge-signal", 0, aCount); RThread t; while (aCount-- > 0) t.RequestSignal(); }

// a guest thread's timed wait: one request signal within aMicroseconds (true, taken) or a timeout (false). Each completion is one signal:
// the timer's own is taken here either way (Cancel completes a pending timer)
static bool BridgeWaitAnyFor(void* aCtx, int aMicroseconds)
	{
	static RTimer timer;
	static TBool created = EFalse;
	if (!created && timer.CreateLocal() != KErrNone) { BridgeWaitAny(aCtx); return true; }
	created = ETrue;
	TRequestStatus st;
	timer.After(st, aMicroseconds);
	SigTrace("timed-wait armed", (TUint)&st, aMicroseconds);
	BridgeWaitAny(aCtx);
	SigTrace("timed-wait woke", (TUint)&st, st.Int());
	if (st != KRequestPending) return false;		// (the signal taken was the timer's; a guest request that also completed is still counted)
	timer.Cancel();
	User::WaitForRequest(st);
	return true;
	}

// a request signal that has already arrived (the guest's thread is preempted, not waiting): taken, or none -- never blocks
static bool BridgePoll(void*)
	{
	if (RThread().RequestCount() <= 0)
		return EFalse;
	User::WaitForAnyRequest();
	SigTrace("took in poll", 0, 0);
	CheckAsync();
	return ETrue;
	}

// microseconds on the system tick: monotonic, unlike the UTC clock (which SNTP moves)
static uint64_t BridgeNowUs(void*)
	{
	static TInt period = 0;
	if (!period) { TTimeIntervalMicroSeconds32 p; UserHal::TickPeriod(p); period = p.Int() > 0 ? p.Int() : 1000; }
	return (uint64_t)User::TickCount() * (uint64_t)period;
	}

// a native DLL's export by ordinal (the GL thunks: libEGL, libGLESv2, ...): each DLL loaded once, kept for the process
static uint32_t BridgeNativeExport(void*, const char* aDll, int aOrdinal)
	{
	struct TNative { TBuf<32> iName; RLibrary iLib; TInt iErr; };
	static TNative libs[8];
	static TInt count = 0;
	static volatile int lockWord = 0;		// guest threads (bridge.cpp) look exports up too: one loader at a time
	struct TSpin { TSpin() { while (__atomic_exchange_n(&lockWord, 1, __ATOMIC_ACQUIRE)) User::AfterHighRes(0); } ~TSpin() { __atomic_store_n(&lockWord, 0, __ATOMIC_RELEASE); } } spin;
	TBuf<32> name;
	name.Copy(TPtrC8((const TUint8*)aDll).Left(32));
	TInt i = 0;
	while (i < count && libs[i].iName.CompareF(name)) ++i;
	if (i == count)
		{
		if (count == 8) return 0;
		new (&libs[i].iLib) RLibrary;
		libs[i].iName = name;
		libs[i].iErr = libs[i].iLib.Load(name);
		if (libs[i].iErr == KErrNone)
			{	// a library handle is the loading thread's: guest threads (bridge.cpp, native threads) look exports up through it too
			RLibrary own = libs[i].iLib;
			if (own.Duplicate(RThread(), EOwnerProcess) == KErrNone) { libs[i].iLib.Close(); libs[i].iLib = own; }
			}
		if (libs[i].iErr != KErrNone)
			RDebug::Printf("PH: proteus native %s not loaded: %d", aDll, libs[i].iErr);
		++count;
		}
	return libs[i].iErr == KErrNone ? (uint32_t)(TUint)libs[i].iLib.Lookup(aOrdinal) : 0;
	}

static uint32_t BridgeNativeExportCtx(void* aCtx, const char* aDll, int aOrdinal) { return BridgeNativeExport(aCtx, aDll, aOrdinal); }

// the guest's cleanup stack (e32base.h / ub_cln.cpp): trap handler = &CTrapCleanup::iHandler (TCleanupTrapHandler: vptr, CCleanup*);
// CCleanup: vptr, iBase, iTop (end of the array), iNext (next free slot); items {TCleanupOperation iOperation, TAny* iPtr} (8 bytes). Logs the top aCount items.
static void DumpCleanup(proteus::CArmCpu& aCpu, proteus::THle& aHle, TInt aCount)
	{
	const TUint32 th = aHle.iExec.iTrapHandler;
	const TUint32 cl = th && aCpu.Valid(th, 8) ? aCpu.Read32(th + 4) : 0;
	if (!cl || !aCpu.Valid(cl, 16)) { RDebug::Printf("PH: proteus cleanup: no CCleanup (handler %x)", th); return; }
	const TUint32 base = aCpu.Read32(cl + 4), top = aCpu.Read32(cl + 8), next = aCpu.Read32(cl + 12);
	RDebug::Printf("PH: proteus cleanup %x: base %x top(end) %x next %x (%d items)", cl, base, top, next, (TInt)(next - base) / 8);
	proteus::TProcess& proc = *aHle.iProcess;
	for (TInt i = 1; i <= aCount && next - 8 * i >= base; ++i)		// iNext = the next free slot
		{
		const TUint32 it = next - 8 * i;
		const TUint32 op = aCpu.Read32(it), ptr = aCpu.Read32(it + 4);
		char w[120];
		proteus::Where(proc, aCpu, op, w, sizeof(w));
		RDebug::Printf("PH: proteus cleanup   top-%d: op %x (%s) ptr %x", i, op, w, ptr);
		}
	}

// (investigation: a UID-range value written = the overwrite under study -> full backtrace)
static TBool aIndexOfInterest(uint32_t aValue) { return (aValue >> 12) == 0x10005; }

static void WatchHit(proteus::CArmCpu& aCpu, uint32_t aAddr, uint32_t aValue, int aSize, void* aHleP)
	{
	static TInt lines = 0;
	if (++lines > 300)
		return;
	proteus::THle& hle = *(proteus::THle*)aHleP;
	char w[120], l[120];
	proteus::Where(*hle.iProcess, aCpu, aCpu.iLastPc, w, sizeof(w));
	proteus::Where(*hle.iProcess, aCpu, aCpu.R[14], l, sizeof(l));
	RDebug::Printf("PH: proteus watch: [%x] <- %x (%d) at %s lr %s", aAddr, aValue, aSize, w, l);
	if (aIndexOfInterest(aValue))
		proteus::Backtrace(aCpu, hle);
	}

// CArmCpu::iOnProgress: every 2^26 instructions, where the guest is (pc, lr); 4000 lines at most
static void Progress(proteus::CArmCpu& aCpu, void* aHleP)
	{
	static TInt lines = 0;
	if (++lines > 4000)
		return;
	const proteus::THle& hle = *(const proteus::THle*)aHleP;
	char pc[120], lr[120];
	proteus::Where(*hle.iProcess, aCpu, aCpu.iLastPc, pc, sizeof(pc));
	proteus::Where(*hle.iProcess, aCpu, aCpu.R[14], lr, sizeof(lr));
	char th[420] = "";
	proteus::ThreadStats(aCpu, const_cast<proteus::THle&>(hle), th, sizeof(th));
	RDebug::Printf("PH: proteus progress %u M instructions: pc %s, lr %s; %s", (TUint)(aCpu.iSteps >> 20), pc, lr, th);
	}

void proteus::JitDiag(const char* aText)
{
	RDebug::Printf("%s", aText);
}

// CArmCpu::iOnForeignPc: the guest called native code -- in practice a virtual function of a native object it shares, like the
// CBitmapFont in the font server's heap (CFbsFont::DoFontSpecInTwips calls Address()->FontSpecInTwips()), whose vtable points into
// the native fntstr.dll. The guest has the ARM twin of that DLL with the same class layouts, so run the twin's function on the
// object: the native function is export i of the native DLL -> export i of the ARM DLL; else slot k of an exported native vtable
// (_ZTV.., the export points at its first slot) -> slot k of the ARM vtable of the same ordinal. 0 = none (the fetch faults).
static uint32_t ForeignTwin(proteus::CArmCpu& aCpu, uint32_t aPc, void* aHleP)
	{
	const proteus::THle& hle = *(const proteus::THle*)aHleP;
	if (const TUint32 stub = proteus::BridgeStubTarget(aPc))		// a reverse stub in a patched vtable slot: its ARM function (bridge.cpp)
		return stub;
	if (!hle.iProcess)
		return 0;
	const TRomHeader* rom = (const TRomHeader*)hle.iRomHeader;
	if (!rom || aPc - rom->iRomBase >= rom->iRomSize)
		return 0;				// not ROM code: nothing native to map
	enum { KCache = 64, KMaxSlots = 64 };
	static TUint32 cachePc[KCache], cacheTo[KCache];
	static TInt cached = 0;
	for (TInt i = 0; i < cached; ++i)
		if (cachePc[i] == aPc) return cacheTo[i];
	const proteus::TProcess& p = *hle.iProcess;
	TUint32 to = 0;
	char how[200] = "";
	for (TInt pass = 0; pass < 2 && !to; ++pass)
		for (TInt m = 1; m < p.iCount && !to; ++m)
			{
			const proteus::TLoaded& md = p.iMod[m];
			if (!md.iExportDir || !md.iExportCount)
				continue;
			TBuf<40> nm;
			nm.Copy(TPtrC8((const TUint8*)md.iName));
			RLibrary lib;
			if (lib.Load(nm) != KErrNone)		// the native DLL of that name (ROM: XIP, so no copy)
				continue;
			for (TUint32 ord = 1; ord <= md.iExportCount && !to; ++ord)
				{
				const TUint32 n = (TUint32)lib.Lookup((TInt)ord);
				if (!n || n - rom->iRomBase >= rom->iRomSize)
					continue;
				if (pass == 0 && (n & ~1u) == (aPc & ~1u))
					{
					to = aCpu.Read32(md.iExportDir + 4 * (ord - 1));
					{ TPtr8 hp((TUint8*)how, sizeof(how) - 1); hp.Format(_L8("%s export %u"), md.iName, (TUint)ord); hp.ZeroTerminate(); }
					}
				for (TInt k = 0; pass == 1 && !to && k < KMaxSlots && n + 4 * k + 4 - rom->iRomBase <= rom->iRomSize; ++k)
					if (*(const TUint32*)(n + 4 * k) == aPc)
						{
						const TUint32 vt = aCpu.Read32(md.iExportDir + 4 * (ord - 1));
						to = vt ? aCpu.Read32(vt + 4 * k) : 0;
						{ TPtr8 hp((TUint8*)how, sizeof(how) - 1); hp.Format(_L8("%s vtable export %u slot %d"), md.iName, (TUint)ord, (int)k); hp.ZeroTerminate(); }
						}
				}
			lib.Close();
			}
	char w[120] = "";
	if (to) proteus::Where(p, aCpu, to, w, sizeof(w));
	RDebug::Printf("PH: proteus native call %08x -> %s%s%s", (TUint)aPc, to ? w : "no ARM twin", to ? " via " : "", how);
	if (cached < KCache)
		{
		cachePc[cached] = aPc;
		cacheTo[cached++] = to;
		}
	return to;
	}

static uint32_t BridgeThreadId() { return (uint32_t)RThread().Id(); }
static uint32_t BridgeGuestAlloc(uint32_t aSize) { return (uint32_t)User::Alloc((TInt)aSize); }
static void BridgeGuestFree(uint32_t aAddr) { User::Free((TAny*)aAddr); }
static void BridgeThreadLog(const char* aLine) { RDebug::Printf("PH: %s", aLine); }
static int32_t BridgeTrapped(void (*aFn)(void*), void* aArg) { TInt r = KErrNone; TRAP(r, aFn(aArg)); return r; }

// ... and with a native euser (bridge.cfg), a native function no ARM twin maps: call it natively (a virtual of a native-created object)
static uint32_t ForeignPc(proteus::CArmCpu& aCpu, uint32_t aPc, void* aHleP)
	{
	const TUint32 to = ForeignTwin(aCpu, aPc, aHleP);
	return to ? to : proteus::BridgeNativeCall(aCpu, aPc);
	}

static void BreakHit(proteus::CArmCpu& aCpu, TInt aIndex, void* aHleP)
	{
	proteus::THle& hle = *(proteus::THle*)aHleP;
	void* aProc = hle.iProcess;
	if (aIndex <= 1)			// breakpoints 0 and 1 also dump the guest's cleanup stack
		DumpCleanup(aCpu, hle, 8);
	if (aIndex <= 1 && aCpu.Valid(aCpu.R[0], 24))		// ... and the 6 words at r0 (the object a method is called on)
		RDebug::Printf("PH: proteus bp %d: [r0] %x %x %x %x %x %x", aIndex, aCpu.Read32(aCpu.R[0]), aCpu.Read32(aCpu.R[0] + 4), aCpu.Read32(aCpu.R[0] + 8),
			aCpu.Read32(aCpu.R[0] + 12), aCpu.Read32(aCpu.R[0] + 16), aCpu.Read32(aCpu.R[0] + 20));
	// breakpoint 0 arms the others (they log only after it was hit); at most 4000 lines
	static TBool armed = EFalse;
	static TInt lines = 0;
	if (aIndex == 0) armed = ETrue;
	if (!armed || ++lines > 4000)
		return;
	char w[120];
	proteus::Where(*(proteus::TProcess*)aProc, aCpu, aCpu.R[14], w, sizeof(w));
	const TUint32 d0 = aCpu.Valid(aCpu.R[1], 8) ? aCpu.Read32(aCpu.R[1]) : 0, d1 = aCpu.Valid(aCpu.R[1], 8) ? aCpu.Read32(aCpu.R[1] + 4) : 0;
	RDebug::Printf("PH: proteus bp %d: r0 %x r1 %x r2 %x r3 %x [r1] %x [r1+4] %x heap %x lr %s", aIndex, aCpu.R[0], aCpu.R[1], aCpu.R[2], aCpu.R[3], d0, d1,
		hle.iExec.iHeap, w);
	}

static TInt MainL()
	{
	TFileName arg;
	User::CommandLine(arg);
	arg.Trim();
#ifdef PROTEUS_JIT
	// "-i" first: the interpreter only (P1 A/B runs; the default is the translator)
	const TBool interpOnly = arg.Left(3) == _L("-i ");
	if (interpOnly)
		{
		arg.Delete(0, 3);
		arg.Trim();
		}
#endif
	// "-b" next: every ARM image through the image broker (the P2e test: what a program without AllFiles gets when PlatSec is enforced)
	if (arg.Left(3) == _L("-b "))
		{
		gViaBroker = ETrue;
		arg.Delete(0, 3);
		arg.Trim();
		}
	// the REAL ARM euser.dll (z:\sys\bin\arm\euser.dll, our device build) + the SWI executive (proteusexec.cpp) whenever it is
	// installed: the way a program started by its name (the loader's redirect) runs. "-h <exe>" = the HLE euser instead (P0's
	// in-process run uses it too); "-r <exe>" = the real one, said explicitly.
	const TBool forceHle = arg.Left(3) == _L("-h ");
	const TBool forceReal = arg.Left(3) == _L("-r ");
	if (forceHle || forceReal)
		{
		arg.Delete(0, 3);
		arg.Trim();
		}
	// "<program> <its command line>" (the loader's redirect: the program's full name never has a space): the rest is the guest's own
	const TInt sp = arg.Locate(' ');
	if (sp >= 0)
		{
		gGuestCmd = arg.Mid(sp + 1);
		arg.SetLength(sp);
		}
	TFileName path = arg.Length() ? arg : TFileName(KDefaultExe);
	RFs fs;
	User::LeaveIfError(fs.Connect());
	CleanupClosePushL(fs);
	TEntry euserEntry;
	// installed = the entry is there, on any drive: Z: in a ROM built with this layer, C: on a PC image with Proteus installed from its
	// SIS (a staged ARM closure). A handler holding only the program's capabilities may not look into \sys\: then the broker answers
	TInt euserState = KErrNotFound;
	for (TInt k = 0; k < 26 && euserState != KErrNone; ++k)
		{
		TChar ch;
		if (RFs::DriveToChar(k == 0 ? EDriveZ : EDriveY - (k - 1), ch) != KErrNone) continue;
		TBuf<32> eu;
		eu.Append(ch);
		eu.Append(_L(":\\sys\\bin\\arm\\euser.dll"));
		euserState = fs.Entry(eu, euserEntry);
		if (euserState == KErrPermissionDenied)
			euserState = ProteusImgExists(eu);
		}
	// bridge.cfg (c:\proteus\ then z:\proteus\): the DLLs whose imports run on the NATIVE library (bridge.cpp, rung R1). A native euser
	// replaces the ARM euser (and the HLE) altogether.
	{
	proteus::BridgeConfigure("");
	TInt bsz = 0;
	RFile bf;
	if (bf.Open(fs, _L("c:\\proteus\\bridge.cfg"), EFileRead) == KErrNone || bf.Open(fs, _L("z:\\proteus\\bridge.cfg"), EFileRead) == KErrNone)
		{
		if (bf.Size(bsz) == KErrNone && bsz > 0 && bsz < 4096)
			{
			TBuf8<4096> cfg;
			if (bf.Read(cfg) == KErrNone)
				{
				cfg.Append(0);
				proteus::BridgeConfigure((const char*)cfg.Ptr());
				}
			}
		bf.Close();
		}
	}
	proteus::BridgeSetLookup(BridgeNativeExportCtx, NULL);
	{	// native threads for guest threads (bridge.cpp): the kernel thread id, guest stacks from the native heap (guest memory under the map)
	proteus::TBridgeThreads thr;
	thr.iThreadId = BridgeThreadId;
	thr.iAlloc = BridgeGuestAlloc;
	thr.iFree = BridgeGuestFree;
#ifdef PROTEUS_JIT
	thr.iJitNew = interpOnly ? NULL : proteus::JitNew;
#else
	thr.iJitNew = NULL;
#endif
	thr.iJitDelete = proteus::JitDelete;
	thr.iLog = BridgeThreadLog;
	thr.iTrapLeave = NULL;
	thr.iTrapped = BridgeTrapped;
	proteus::BridgeSetThreads(thr);
	}
	const TBool nativeEuser = proteus::BridgeNative("euser.dll");
	const TBool realEuser = !nativeEuser && (forceReal || (!forceHle && arg.Length() && euserState == KErrNone));
	RDebug::Printf("PH: proteus euser: %s", nativeEuser ? "NATIVE (bridge.cfg)" : realEuser ? "real ARM" : "HLE");
	RFile f;
	TInt r = OpenImage(fs, path, f);
	RDebug::Printf("PH: proteus open %S -> %d", TLog8(path).P(), r);
	User::LeaveIfError(r);
	CleanupClosePushL(f);
	TInt size = 0;
	User::LeaveIfError(f.Size(size));
	HBufC8* img = HBufC8::NewLC(size);
	TPtr8 ip = img->Des();
	r = f.Read(0, ip);
	User::LeaveIfError(r);
	// the guest region: its own local disconnected chunk, the largest reservation the kernel grants (KGuestBytesMax, halved while it
	// refuses); the kernel hands out committed pages cleared
	RChunk chunk;
	TInt guestBytes = KGuestBytesMax;
	while ((r = chunk.CreateDisconnectedLocal(0, 0, guestBytes)) != KErrNone && guestBytes > KGuestBytesMin)
		guestBytes >>= 1;
	User::LeaveIfError(r);
	if (guestBytes - KNativeReserveBytes >= KGuestBytesMin)		// leave the native services their room (KNativeReserveBytes)
		{
		chunk.Close();
		guestBytes -= KNativeReserveBytes;
		User::LeaveIfError(chunk.CreateDisconnectedLocal(0, 0, guestBytes));
		}
	RDebug::Printf("PH: proteus guest region %d MB at %08x", guestBytes >> 20, (TUint32)chunk.Base());
	CleanupClosePushL(chunk);
	TUint8* mem = chunk.Base();
	proteus::CArmCpu* cpu = new (ELeave) proteus::CArmCpu;
	cpu->iHost = mem;
	cpu->iBase = (TUint32)mem;
	cpu->iSize = guestBytes;
	cpu->iCommit = CommitGuest;
	cpu->iCommitCtx = &chunk;
	cpu->iPageMap = (TUint8*)User::AllocZL(guestBytes >> 12);
	CleanupStack::PushL(cpu->iPageMap);
#ifdef PROTEUS_JIT
	// P1: dynarmic runs the guest (armjit.cpp); it follows every Commit from here on. Null = the interpreter.
	// c:\proteus\interp.txt = the interpreter for every program of this disk (app A/B runs: an app started by its name comes through
	// the loader's redirect, whose command line has no room for "-i")
	TEntry interpEntry;
	const TBool interpDisk = fs.Entry(_L("c:\\proteus\\interp.txt"), interpEntry) == KErrNone;
	cpu->iJit = interpOnly || interpDisk ? NULL : proteus::JitNew(*cpu);
	RDebug::Printf("PH: proteus cpu: %s", cpu->iJit ? "jit" : "interpreter");
#endif
	{
	// bridge.cfg "map": the whole x32 user address space is guest memory, as it is (a native object's address is the guest's)
	uint32_t mlo, mhi;
	if (proteus::BridgeMapWanted(mlo, mhi) && cpu->AddWindow(mlo, mhi - mlo))
		{
#ifdef PROTEUS_JIT
		proteus::JitMap(cpu->iJit, mlo, mhi - mlo);
#endif
		RDebug::Printf("PH: proteus bridge: guest memory = native %08x..%08x", mlo, mhi);
		}
	}
	// the main thread's stack at the region top (proteusrt.cpp StartThreadStack: the thread data 4 KB below the top)
	if (!cpu->Commit(cpu->iBase + guestBytes - proteus::KStackBytes, proteus::KStackBytes))
		User::Leave(KErrNoMemory);
	RPointerArray<HBufC8> dllBufs;
	CleanupStack::PushL(TCleanupItem((TCleanupOperation)&ResetDestroy, &dllBufs));
	TFindCtx fc;
	fc.iFs = &fs;
	fc.iBufs = &dllBufs;
	TParsePtrC pp(path);
	fc.iDir = pp.DriveAndPath();
	if (pp.Path().CompareF(_L("\\sys\\bin\\")) == 0)
		fc.iDir.Append(_L("arm\\"));		// an installed EXE (\sys\bin\, next to the native ones): its ARM DLLs live in the per-arch directory
	proteus::TProcess* proc = new (ELeave) proteus::TProcess;
	proc->iRealEuser = realEuser;
	const TUint32 at = (cpu->iBase + 0x1000 + 0xFFF) & ~0xFFFu;
	if (!proteus::LoadProcess(*cpu, img->Ptr(), size, at, FindDll, &fc, *proc))
		{
		RDebug::Printf("PH: proteus load failed: %s", proc->iError);
		proteus::FreeProcess(*proc);
		delete proc;
		proteus::JitDelete(cpu->iJit);
		delete cpu;
		User::Leave(KErrNotSupported);
		}
	proteus::TLoaded& ld = proc->iMod[0];
	for (TInt m = 1; m < proc->iCount; ++m)
		RDebug::Printf("PH: proteus linked ARM DLL %s at %x (%d exports)", proc->iMod[m].iName, proc->iMod[m].iLoadAddr, (TInt)proc->iMod[m].iExportCount);
	RDebug::Printf("PH: proteus loaded: cpu %x uid3 %x code at %x entry %x imports from %s", ld.iCpu, ld.iUid3, ld.iLoadAddr, ld.iEntry, ld.iDll[0]);
	proteus::THle hle;
	hle.iImage = &ld;
	hle.iProcess = proc;
	hle.iLog = Log;
	hle.iLogCtx = NULL;
	hle.iUnimplemented = 0;
	TBridge* bridge = new (ELeave) TBridge;
	bridge->iSid = size >= 132 ? (img->Ptr()[128] | (img->Ptr()[129] << 8) | (img->Ptr()[130] << 16) | ((TUint32)img->Ptr()[131] << 24)) : 0;	// E32ImageHeader iS.iSecureId
	bridge->iFs = &fs;
	for (TInt i = 0; i < KMaxBridge; ++i)
		{
		bridge->iPathSet[i] = EFalse;
		bridge->iCtl[i] = -1;
		bridge->iUsed[i] = bridge->iConnected[i] = bridge->iPropUsed[i] = EFalse;
		bridge->iObjType[i] = 0;
		bridge->iObjNative[i] = 0;
		}
	proteus::TIpcBridge ipc = { bridge, BridgeCreate, BridgeSend, BridgeOwns, BridgeClose, BridgeShare, BridgeWaitAny, BridgeSignal, BridgeKernel, BridgeWaitAnyFor, BridgeNowUs,
		BridgeNativeExport, BridgePoll, BridgeSyncDone, CtlHostThread, CtlHostStart, CtlHostExit, CtlHostKill, CtlSessionFromServer };
	hle.iIpc = &ipc;
	TBuf8<KMaxFileName + 1> exePath;
	exePath.Copy(path);
	// a test closure staged in <drive>:\proteus\ (DISKFILES) stands for an app installed in \sys\bin\: report z:\sys\bin\<name> as the process
	// file name, so apparc / CEikApplication find its resources on Z: (\resource\apps\<name>.rsc) as for the native app
	if (path.FindF(_L("\\proteus\\")) >= 0)
		{
		exePath = _L8("z:\\sys\\bin\\");
		exePath.Append(TParsePtrC(path).NameAndExt());
		}
	hle.iExePath = (const char*)exePath.PtrZ();
	hle.iFind = FindDll;
	hle.iFindCtx = &fc;
	hle.iThreadId = (TUint32)RThread().Id();
	// debug breakpoints (local investigations): <exe dir>\break.txt, lines "<module> <hex offset>" -> log r0-r3, [r1], [r1+4], lr at that pc
	{
		TFileName bpf(fc.iDir);
		bpf.Append(_L("break.txt"));
		RFile bf;
		if (bf.Open(fs, bpf, EFileRead | EFileShareReadersOnly) == KErrNone)
			{
			TBuf8<512> txt;
			bf.Read(txt);
			bf.Close();
			TLex8 lex(txt);
			while (!lex.Eos() && cpu->iBpCount < proteus::CArmCpu::KMaxBp)
				{
				TPtrC8 mod = lex.NextToken();
				lex.SkipSpace();
				if (mod == _L8("watch"))		// "watch <hex address> <hex length>": log every store into that range
					{
					TUint32 wa = 0, wl = 4;
					if (lex.Peek() == '0') { lex.Inc(); if (lex.Peek() == 'x') lex.Inc(); }
					lex.Val(wa, EHex);
					lex.SkipSpace();
					if (lex.Peek() == '0') { lex.Inc(); if (lex.Peek() == 'x') lex.Inc(); }
					lex.Val(wl, EHex);
					cpu->iWatchLo = wa;
					cpu->iWatchHi = wa + wl;
					cpu->iOnWatch = WatchHit;
					RDebug::Printf("PH: proteus watch %x..%x", wa, wa + wl);
					continue;
					}
				const TBool byOrdinal = lex.Peek() == '@';		// "@<ordinal>" (decimal) = that export
				if (byOrdinal) lex.Inc();
				if (lex.Peek() == '0') { lex.Inc(); if (lex.Peek() == 'x' || lex.Peek() == 'X') lex.Inc(); }
				TUint32 off = 0;
				if (mod.Length() == 0 || lex.Val(off, byOrdinal ? EDecimal : EHex) != KErrNone)
					break;
				for (TInt m = 0; m < proc->iCount; ++m)
					if (TPtrC8((const TUint8*)(m ? proc->iMod[m].iName : "exe")).CompareF(mod) == 0)
						{
						const proteus::TLoaded& md = proc->iMod[m];
						if (byOrdinal && (off < 1 || off > md.iExportCount))
							break;
						cpu->iBp[cpu->iBpCount++] = byOrdinal ? (cpu->Read32(md.iExportDir + 4 * (off - 1)) & ~1u) : md.iLoadAddr + off;
						RDebug::Printf("PH: proteus breakpoint %d at %S+%x", cpu->iBpCount - 1, &mod, off);
						}
				}
			if (cpu->iBpCount)
				cpu->iOnBp = BreakHit;
			}
	}
	cpu->iBpCtx = &hle;
	if (hle.iProcess)
		cpu->iOnProgress = Progress;
	cpu->iOnForeignPc = ForeignPc;
	// the ROM is mapped (read-only) in every process: a window, so guest code can read ROM-resident files (bafl resources) as on the device
	const TRomHeader* rom = (const TRomHeader*)UserSvr::RomHeaderAddress();
	hle.iRomHeader = (TUint32)rom;
	if (rom)
		{
		cpu->AddWindow((TUint32)rom->iRomBase, rom->iRomSize);
		RDebug::Printf("PH: proteus ROM window %x + %x", (TUint)rom->iRomBase, rom->iRomSize);
		}
	TTrapHandler* const ownHandler = User::TrapHandler();	// a native euser (bridge.cfg): the guest's CTrapCleanup::New installs ITS handler on this thread
	proteus::RunExe(*cpu, hle, ~0ull);		// no instruction budget for a real app (it was 50 M: about.exe hit it while drawing text; 4 G: Sparkle hit that on the x32 PC VM)
	RDebug::Printf("PH: proteus guest halted: %d exit %d after %d instructions, fault %x '%s'", (TInt)cpu->iHalt, cpu->iExitCode, (TInt)cpu->iSteps, cpu->iFaultAddr, cpu->iNote);
	if (nativeEuser && User::TrapHandler() != ownHandler)
		{	// a guest stopped before it deleted its CTrapCleanup: our PopAndDestroy below would pop the GUEST's items (ARM cleanup functions
		// behind reverse stubs of a CPU about to go) -- the KERN-EXEC 3 after a halted PyS60
		User::SetTrapHandler(ownHandler);
		RDebug::Printf("PH: proteus trap handler restored (the guest's cleanup stack was still installed)");
		}
	if (cpu->iHalt == proteus::EUndefined)
		for (TInt m = 0; m < proc->iCount; ++m)
			{
			const proteus::TLoaded& md = proc->iMod[m];
			if (cpu->iFaultAddr >= md.iLoadAddr && cpu->iFaultAddr < md.iEnd)
				RDebug::Printf("PH: proteus undefined insn %08x at %s+%x", cpu->Read32(cpu->iFaultAddr), m ? md.iName : "exe", cpu->iFaultAddr - md.iLoadAddr);
			}
	if (cpu->iHalt != proteus::EExited)
		{
		char w[3][120];
		proteus::Where(*proc, *cpu, cpu->iLastPc, w[0], sizeof(w[0]));
		proteus::Where(*proc, *cpu, cpu->R[15], w[1], sizeof(w[1]));
		proteus::Where(*proc, *cpu, cpu->R[14], w[2], sizeof(w[2]));
		RDebug::Printf("PH: proteus halt at %s", w[0]);
		RDebug::Printf("PH: proteus   pc -> %s", w[1]);
		RDebug::Printf("PH: proteus   lr = %s", w[2]);
		proteus::Backtrace(*cpu, hle);
		if (nativeEuser)
			{	// the native euser holds objects the halted guest made (its scheduler, active objects with patched ARM vtables, threads on
			// other CPUs): no teardown of them or of the guest memory under them; the process ends here, with a reason
			const TInt reason = cpu->iHalt == proteus::EHleStop || cpu->iHalt == proteus::ESvcUnhandled ? KErrNotSupported : KErrGeneral;
			RDebug::Printf("PH: proteus exit %d: guest halt %d '%s' (native euser: no teardown)", reason, (TInt)cpu->iHalt, cpu->iNote);
			User::Exit(reason);
			}
		}
	const TBool ok = cpu->iHalt == proteus::EExited && cpu->iExitCode == 0;
	for (TInt i = 0; i < KMaxBridge; ++i)
		if (bridge->iUsed[i] && bridge->iConnected[i])
			bridge->iSession[i].Close();
	for (TInt i = 0; i < KMaxBridge; ++i)
		{
		if (bridge->iPropUsed[i])
			bridge->iProp[i].Close();
		if (bridge->iObjType[i])
			bridge->iObj[i].Close();
		}
	delete bridge;
	proteus::JitDelete(cpu->iJit);
	delete cpu;
	proteus::FreeProcess(*proc);
	delete proc;
	CleanupStack::PopAndDestroy(6, &fs);		// dllBufs, the page map, chunk, img, f, fs
	RDebug::Printf(arg.Length() ? "PH: proteus guest run %s" : "PH: proteus P0 %s", ok ? "PASS" : "FAIL");
	return ok ? KErrNone : KErrGeneral;
	}

// P1: launch the ARM EXE as a plain EXE by name; the loader (core sf_lepoc.cpp ForeignCpuRedirect) turns it into
// cpuhandler_arm.exe <path>. Exit reason 0 (proteus returns KErrNone only if the guest exited 0) = pass.
// proteus.exe <args> as a child; its exit reason (0 = the guest exited 0)
static TInt SpawnProteus(const TDesC& aArgs)
	{
	RProcess q;
	TInt r = q.Create(_L("cpuhandler_arm.exe"), aArgs);
	if (r != KErrNone)
		return r;
	TRequestStatus st;
	q.Logon(st);
	q.Resume();
	User::WaitForRequest(st);
	r = q.ExitType() == EExitKill ? q.ExitReason() : KErrGeneral;
	q.Close();
	return r;
	}

static void ResetDestroy(TAny* aArray)
	{
	((RPointerArray<HBufC8>*)aArray)->ResetAndDestroy();
	}

static TInt SpawnPlain(const TDesC& aName, const char* aPhase)
	{
	RProcess p;
	TInt r = p.Create(aName, KNullDesC);
	RDebug::Printf("PH: proteus %s RProcess::Create(%S) -> %d", aPhase, TLog8(aName).P(), r);
	if (r != KErrNone)
		return r;
	TRequestStatus st;
	p.Logon(st);
	p.Resume();
	User::WaitForRequest(st);
	const TInt reason = p.ExitReason();
	RDebug::Printf("PH: proteus %s child exit type %d reason %d", aPhase, (TInt)p.ExitType(), reason);
	const TBool ok = p.ExitType() == EExitKill && reason == KErrNone;
	p.Close();
	return ok ? KErrNone : KErrGeneral;
	}

// start aName and wait up to aSeconds for it; a process still running then is left running (aExit = EExitPending)
static TInt RunTimed(const TDesC& aName, const TDesC& aArgs, TInt aSeconds, TExitType& aExit, TInt& aReason, const TDesC* aAppName = NULL)
	{
	aExit = EExitPending;
	aReason = 0;
	RProcess p;
	TInt r = p.Create(aName, aArgs);
	if (r != KErrNone)
		return r;
	if (aAppName)		// started as the menu starts an app (apparc: a CApaCommandLine naming the app's EXE in the new process's environment slots)
		{
		TRAP(r, {
			CApaCommandLine* cmd = CApaCommandLine::NewLC();
			cmd->SetExecutableNameL(*aAppName);
			cmd->SetCommandL(EApaCommandRun);
			cmd->SetProcessEnvironmentL(p);
			CleanupStack::PopAndDestroy(cmd);
			});
		if (r != KErrNone)
			{
			p.Kill(r);
			p.Close();
			return r;
			}
		}
	TRequestStatus st, tst;
	RTimer t;
	t.CreateLocal();
	p.Logon(st);
	p.Resume();
	// RTimer::After takes a 32-bit microsecond interval (35 min at most): wait in chunks of 1800 s up to aSeconds
	TInt left = aSeconds;
	while (st == KRequestPending && left > 0)
		{
		const TInt chunk = left > 1800 ? 1800 : left;
		t.After(tst, chunk * 1000000);
		User::WaitForRequest(st, tst);
		if (st == KRequestPending)
			left -= chunk;		// the chunk elapsed
		else
			{
			t.Cancel();
			User::WaitForRequest(tst);
			}
		}
	if (st == KRequestPending)
		{
		p.LogonCancel(st);
		User::WaitForRequest(st);
		}
	else
		{
		aExit = p.ExitType();
		aReason = p.ExitReason();
		}
	t.Close();
	p.Close();
	return KErrNone;
	}

TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RDebug::Printf("PH: proteus start");
	TRAPD(r, r = MainL());
	if (r != KErrNone)
		RDebug::Printf("PH: proteus leave %d FAIL", r);
	TFileName arg;
	User::CommandLine(arg);
	arg.Trim();
	if (r == KErrNone && arg.Length() == 0)		// the P0 test run (no argument): now the P1 one
		{
		const TInt p1 = SpawnPlain(_L("armhello.exe"), "P1");
		RDebug::Printf("PH: proteus P1 %s", p1 == KErrNone ? "PASS" : "FAIL");
		// P2: an ARM EXE that imports an ARM DLL (proteusdll.dll, staged byte-pair compressed), both in sys/bin/arm: proteus links the DLL
		// into the guest and runs its entry point (static constructor, writable static data)
		const TInt p2 = SpawnPlain(_L("proteusdllexe.exe"), "P2");
		RDebug::Printf("PH: proteus P2 %s", p2 == KErrNone ? "PASS" : "FAIL");
		// P2b: the EXE byte-pair compressed too (emitter/e32image.py compress)
		const TInt p2b = SpawnPlain(_L("proteusdllbp.exe"), "P2b");
		RDebug::Printf("PH: proteus P2b %s", p2b == KErrNone ? "PASS" : "FAIL");
		// P2c: the same EXE + DLL on the REAL ARM euser (executive calls emulated): proteus.exe -r <exe>
		const TInt p2c = SpawnProteus(_L("-r z:\\sys\\bin\\proteusdllbp.exe"));
		RDebug::Printf("PH: proteus P2c (real ARM euser) %s", p2c == KErrNone ? "PASS" : "FAIL");
		// P2d: ARM code -> the ARM efsrv -> the IPC bridge -> the native file server
		const TInt p2d = SpawnProteus(_L("-r z:\\sys\\bin\\arm\\proteusfs.exe"));
		RDebug::Printf("PH: proteus P2d (ARM efsrv over the IPC bridge) %s", p2d == KErrNone ? "PASS" : "FAIL");
		// P2f: the same, every ARM image through the image broker (proteusimg.exe: the handler without AllFiles)
		const TInt p2f = SpawnProteus(_L("-b -r z:\\sys\\bin\\arm\\proteusfs.exe"));
		RDebug::Printf("PH: proteus P2f (ARM images through the image broker) %s", p2f == KErrNone ? "PASS" : "FAIL");
		// P2e (only when staged, e.g. DISKFILES=<dir>=proteus): a real app + its ARM DLL closure in c:\proteus\ -- report how far it gets
		RFs fs;
		TEntry ent;
		if (fs.Connect() == KErrNone)
			{
			// c:\proteus\app.txt (optional) = the exe name to run instead of about.exe (rung 29: a second app)
			TBuf8<64> app(_L8("about.exe"));
				{
				RFile af;
				if (af.Open(fs, _L("c:\\proteus\\app.txt"), EFileRead | EFileShareReadersOnly) == KErrNone)
					{
					TBuf8<64> a8;
					af.Read(a8);
					af.Close();
					TInt n = 0;
					while (n < a8.Length() && a8[n] > ' ') ++n;
					if (n > 0) app.Copy(a8.Left(n));
					}
				}
			TFileName appPath(_L("c:\\proteus\\"));
			TBuf<64> app16;
			app16.Copy(app);
			appPath.Append(app16);
			if (fs.Entry(appPath, ent) == KErrNone)
				{
				RProcess q;
				TFileName cmd(_L("-r "));
				cmd.Append(appPath);
				TInt r = q.Create(_L("cpuhandler_arm.exe"), cmd);
				RDebug::Printf("PH: proteus P2e %S -> %d", &app, r);
				if (r == KErrNone)
					{
					TRequestStatus st, tst;
					RTimer t;
					t.CreateLocal();
					q.Logon(st);
					q.Resume();
					t.After(tst, 840 * 1000 * 1000);		// 14 min: about.exe loads its long text incrementally at interpreter speed (rung 22)
					User::WaitForRequest(st, tst);
					if (st == KRequestPending)
						{
						RDebug::Printf("PH: proteus P2e %S still running after 840 s (blocked in the guest or busy) -- killed", &app);
						q.Kill(KErrTimedOut);
						User::WaitForRequest(st);
						}
					else
						{
						t.Cancel();
						User::WaitForRequest(tst);
						}
					RDebug::Printf("PH: proteus P2e %S exit type %d reason %d", &app, (TInt)q.ExitType(), q.ExitReason());
					t.Close();
					q.Close();
					}
				}
			// P3 (only when staged: c:\sis\*.sis, installed in name order, + c:\sisapp.txt = the EXE to start): real ARM packages through the
			// real installer (sisinst = the silent install pheget uses), then the app started by its plain name with apparc's command line, as the
			// menu does (the loader hook)
			CDir* sis = NULL;
			TEntry sisAppEntry;
			const TBool sisStaged = fs.GetDir(_L("c:\\sis\\*.sis"), KEntryAttNormal, ESortByName, sis) == KErrNone && sis->Count();
			// (c:\sisapp.txt alone = the packages are installed already: only the app is started)
			if (sisStaged || fs.Entry(_L("c:\\sisapp.txt"), sisAppEntry) == KErrNone)
				{
				TExitType et = EExitKill;
				TInt reason = KErrNone, ir = KErrNone;
				for (TInt i = 0; sisStaged && i < sis->Count() && ir == KErrNone && et == EExitKill && reason == KErrNone; ++i)
					{
					TFileName pkg(_L("c:\\sis\\"));
					pkg.Append((*sis)[i].iName);
					ir = RunTimed(_L("sisinst.exe"), pkg, 14400, et, reason);
					RDebug::Printf("PH: proteus P3 install %S: create %d exit type %d reason %d", TLog8(pkg).P(), ir, (TInt)et, reason);
					}
				TBuf8<64> exe8;
				RFile af;
				if (af.Open(fs, _L("c:\\sisapp.txt"), EFileRead | EFileShareReadersOnly) == KErrNone)
					{
					af.Read(exe8);
					af.Close();
					TInt n = 0;
					while (n < exe8.Length() && exe8[n] > ' ') ++n;
					exe8.SetLength(n);
					}
				if (ir == KErrNone && et == EExitKill && reason == KErrNone && exe8.Length())
					{
					TBuf<64> exe;
					exe.Copy(exe8);
					// a GUI app runs until it is closed: PASS = still running after the wait AND a window group of its own in the native WSERV
					// (CApaWindowGroupName carries the app UID as 8 hex digits) -- a guest that only blocks somewhere has none
					TFileName app(_L("c:\\sys\\bin\\"));		// where the installer put it (the app list's executable name)
					app.Append(exe);
					const TInt ar = RunTimed(exe, KNullDesC, 240, et, reason, &app);
					const TBool running = ar == KErrNone && et == EExitPending;
					RDebug::Printf("PH: proteus P3 %S: create %d exit type %d reason %d%s", &exe8, ar, (TInt)et, reason, running ? " (running after 240 s)" : "");
					TBool hasWg = EFalse;
					TFileName exePath(_L("c:\\sys\\bin\\"));
					exePath.Append(exe);
					TBuf8<12> hdr;
					RFile ef;
					if (running && ef.Open(fs, exePath, EFileRead | EFileShareReadersOnly) == KErrNone)
						{
						ef.Read(hdr, 12);
						ef.Close();
						}
					RWsSession ws;
					if (hdr.Length() == 12 && ws.Connect() == KErrNone)
						{
						TBuf<8> uid;
						uid.NumFixedWidth(hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | ((TUint)hdr[11] << 24), EHex, 8);
						CArrayFixFlat<TInt>* wgs = new CArrayFixFlat<TInt>(8);
						if (wgs && ws.WindowGroupList(wgs) == KErrNone)
							for (TInt i = 0; i < wgs->Count() && !hasWg; ++i)
								{
								TBuf<256> nm;
								if (ws.GetWindowGroupNameFromIdentifier((*wgs)[i], nm) == KErrNone && nm.FindF(uid) >= 0)
									{
									hasWg = ETrue;
									RDebug::Printf("PH: proteus P3 window group %d: %S", (*wgs)[i], TLog8(nm).P());
									}
								}
						delete wgs;
						ws.Close();
						RDebug::Printf("PH: proteus P3 app uid %S: %s", TLog8(uid).P(), hasWg ? "has a window group" : "no window group");
						}
					RDebug::Printf("PH: proteus P3 (installed ARM app) %s", running && hasWg ? "PASS" : "FAIL");
					}
				else
					RDebug::Printf("PH: proteus P3 (installed ARM app) FAIL");
				}
			delete sis;
			fs.Close();
			}
		}
	delete cleanup;
	return r;
	}

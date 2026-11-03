// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteus.cpp -- OURS (Proteus P0, docs/phorepoc/research/proteus-x86.md): proteus.exe, the first Proteus component on the x86/x32 board: a USER-SIDE loader + ARM interpreter that
// runs a real ARM Symbian EXE (an E32Image with CPU id ARMv5) inside one x32 process. proteus.exe [<arm exe>] (default z:\sys\bin\arm\proteushello.exe: the ARM files live in the
// per-arch directory \sys\bin\arm\ of the design). The guest's memory is one block of this process's heap (guest address == host address under ILP32); the ARM code's euser imports are
// emulated by ordinal (proteusrt.cpp). Prints "PH: proteus ..." lines. P0 stand-in for the loader hook (f32image.h iCpuIdentifier) + dynarmic of P1.
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>
#include <e32property.h>
#include <w32std.h>
#include <e32rom.h>
#include <e32svr.h>
#include <string.h>
#include "armcpu.h"
#include "proteusrt.h"
#include "../proteusimg/proteusimg.h"

_LIT(KArmDirZ, "z:\\sys\\bin\\arm\\");
static void ResetDestroy(TAny* aArray);
_LIT(KDefaultExe, "z:\\sys\\bin\\arm\\proteushello.exe");
// the guest region: reserved address space, committed as the guest uses it (modules as they load, the main stack, chunk pages as the guest
// commits them) -- a real app's heap reserves its EPOCHEAPSIZE maximum (PyS60: 32 MB) and commits a little of it
const TInt KGuestBytes = 256 << 20;

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
	// the per-arch directories first; then \sys\bin\ of every drive in the loader's order (Y: .. A:, Z:), where a SIS installs an app's ARM
	// DLLs next to the native ones: only a file whose E32Image header names an ARM CPU (offset 122) is taken there
	TFileName path;
	for (TInt i = 0; i < 2 + 26; ++i)
		{
		if (i == 0) path = c.iDir;
		else if (i == 1) path = KArmDirZ;
		else
			{
			const TInt d = i - 2 < 25 ? EDriveY - (i - 2) : EDriveZ;
			TChar ch;
			if (RFs::DriveToChar(d, ch) != KErrNone) continue;
			path.SetLength(0);
			path.Append(ch);
			path.Append(_L(":\\sys\\bin\\"));
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
		if (r == KErrNone && i >= 2 && (bp.Length() < 124 || (bp[122] | (bp[123] << 8)) < 0x2000 || (bp[122] | (bp[123] << 8)) > 0x2002))
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
	RProperty iProp[KMaxBridge];				// guest RProperty handles = KPropHandle0 + i
	TBool iPropUsed[KMaxBridge];
	RChunk iObj[KMaxBridge];					// P2: kernel objects opened by name (chunk / mutex / semaphore: all an RHandleBase), KObjHandle0 + i
	TInt iObjType[KMaxBridge];					// TObjectType, 0 = free slot (EThread is never opened here)
	// the guest's file-server identity: \private\<its SecureId>\ (sessions start there, and the private-path calls answer it --
	// BridgeFsPrivate; the same as the server's own answers when the loader gave this process the program's SecureId)
	TUint32 iSid;
	TBool iPathSet[KMaxBridge];
	RFs* iFs;
	};
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
	return (aHandle & 0xFFFF0000u) == KObjHandle0 && i < KMaxBridge && b.iObjType[i] ? i : -1;
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
	if (n == _L8("EExecPropertyCancel")) { p.Cancel(); return true; }
	return false;
	}

static TInt BridgeIndex(TBridge& b, uint32_t aHandle)
	{
	// (subsessions keep the session handle with KHandleNoClose 0x8000 set: RSubSessionBase::DoCreateSubSession)
	const TInt i = (TInt)(aHandle & 0x7FFF);
	return (aHandle & 0xFFFF0000u) == KBridgeHandle0 && i < KMaxBridge && b.iUsed[i] ? i : -1;
	}

static bool BridgeProperty(void* aCtx, const char* aExec, uint32_t* r);

// kernel objects for the guest: P&S, OpenObject by name, and the calls on the opened chunks / mutexes / semaphores (native, blocking)
static bool BridgeKernel(void* aCtx, const char* aExec, uint32_t* r, proteus::CArmCpu& aCpu)
	{
	TBridge& b = *(TBridge*)aCtx;
	const TPtrC8 n((const TUint8*)aExec);
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
	const TInt i = ObjIndex(b, r[0]);
	if (i < 0)
		return false;
	RChunk& c = b.iObj[i];
	if (n == _L8("EExecHandleClose")) { c.Close(); b.iObjType[i] = 0; r[0] = KErrNone; return true; }
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
	if (n == _L8("EExecTimerCancel")) { tm.Cancel(); return true; }
	if (n == _L8("EExecChangeNotifierLogon")) { r[0] = (uint32_t)reinterpret_cast<RChangeNotifier&>(c).Logon(*(TRequestStatus*)r[1]); return true; }
	if (n == _L8("EExecChangeNotifierLogoff")) { r[0] = (uint32_t)reinterpret_cast<RChangeNotifier&>(c).LogonCancel(); return true; }
	if (n == _L8("EExecChunkBase")) { r[0] = (uint32_t)c.Base(); return true; }
	if (n == _L8("EExecChunkSize")) { r[0] = (uint32_t)c.Size(); return true; }
	if (n == _L8("EExecChunkMaxSize")) { r[0] = (uint32_t)c.MaxSize(); return true; }
	if (n == _L8("EExecChunkBottom")) { r[0] = (uint32_t)c.Bottom(); return true; }
	if (n == _L8("EExecChunkTop")) { r[0] = (uint32_t)c.Top(); return true; }
	if (n == _L8("EExecMutexWait")) { reinterpret_cast<RMutex&>(c).Wait(); r[0] = KErrNone; return true; }
	if (n == _L8("EExecMutexSignal")) { reinterpret_cast<RMutex&>(c).Signal(); return true; }
	if (n == _L8("EExecMutexIsHeld")) { r[0] = reinterpret_cast<RMutex&>(c).IsHeld(); return true; }
	if (n == _L8("EExecSemaphoreWait")) { reinterpret_cast<RSemaphore&>(c).Wait(); r[0] = KErrNone; return true; }
	if (n == _L8("EExecSemaphoreSignal1")) { reinterpret_cast<RSemaphore&>(c).Signal(); return true; }
	if (n == _L8("EExecSemaphoreSignalN")) { reinterpret_cast<RSemaphore&>(c).Signal((TInt)r[1]); return true; }
	RDebug::Printf("PH: proteus kernel: %s on an opened object is not bridged", aExec);
	return false;
	}

static bool BridgeOwns(void* aCtx, uint32_t aHandle) { return BridgeIndex(*(TBridge*)aCtx, aHandle) >= 0 || PropIndex(*(TBridge*)aCtx, aHandle) >= 0; }

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
			return KBridgeHandle0 + i;
			}
	return KErrNoMemory;
	}

static void WatchAsync(TInt aFn, TRequestStatus* aSt);
// a synchronous guest send (SessionSendSync, e.g. sessions with 0 async slots) is done synchronously here -- an async native send would
// need a slot (KErrServerBusy) -- and the guest's TRequestStatus is completed + this thread signalled, as the kernel would
static void CompleteGuest(TRequestStatus* aStatus, TInt aResult)
	{
	if (!aStatus)
		return;
	*aStatus = aResult;
	RThread().RequestSignal();
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
	const TInt i = BridgeIndex(b, aHandle);
	if (i < 0)
		return KErrBadHandle;
	TRequestStatus* st = (TRequestStatus*)aStatus;
	TIpcArgs args;
	if (aArgs)
		Mem::Copy(&args, (const TAny*)aArgs, sizeof(TIpcArgs));		// iArgs[4] + iFlags: the guest's own descriptors, in this process
	// window-server trace (the first 120 messages): function, and for a command buffer (a TDesC8 in arg 0) its size and first opcodes
	// (each command: TUint16 opcode, TInt16 length, [TInt32 handle if opcode bit 15], data)
	static TInt wsLines = 0;
	if (b.iName[i] == _L("!Windowserver") && wsLines < 300 && b.iConnected[i])
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
		if (wsLines < 120 || aFunction >= 0x10000 || redraw)
			{
			++wsLines;
			RDebug::Printf("PH: proteus WS: fn %x len %d%S", aFunction, len, &ops);
			}
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
		CompleteGuest(st, b.iSession[i].Sync(aFunction, args));
		return KErrNone;
		}
	if (st)
		{
		b.iSession[i].Async(aFunction, args, *st);
		if (b.iName[i] == _L("!Windowserver"))
			WatchAsync(aFunction, st);
		return KErrNone;
		}
	return b.iSession[i].Blind(aFunction, args);
	}

static int BridgeClose(void* aCtx, uint32_t aHandle)
	{
	TBridge& b = *(TBridge*)aCtx;
	const TInt pi = PropIndex(b, aHandle);
	if (pi >= 0)
		{
		b.iProp[pi].Close();
		b.iPropUsed[pi] = EFalse;
		return KErrNone;
		}
	const TInt i = BridgeIndex(b, aHandle);
	if (i < 0)
		return KErrBadHandle;
	if (aHandle & 0x8000)		// KHandleNoClose
		return KErrNone;
	if (b.iConnected[i])
		b.iSession[i].Close();
	b.iUsed[i] = EFalse;
	return KErrNone;
	}

static int BridgeShare(void* aCtx, uint32_t aHandle, int)
	{
	TBridge& b = *(TBridge*)aCtx;
	const TInt i = BridgeIndex(b, aHandle);
	return i >= 0 && b.iConnected[i] ? b.iSession[i].ShareAuto() : KErrNone;
	}

// window-server async requests of the guest (EventReady, RedrawReady, ...): remember the status, report when the native WSERV completes it
struct TAsyncWatch { TInt iFn; TRequestStatus* iSt; };
static TAsyncWatch gAsync[16];
static void WatchAsync(TInt aFn, TRequestStatus* aSt)
	{
	for (TInt k = 0; k < 16; ++k)
		if (!gAsync[k].iSt || gAsync[k].iSt == aSt) { gAsync[k].iFn = aFn; gAsync[k].iSt = aSt; return; }
	}
static void CheckAsync()
	{
	for (TInt k = 0; k < 16; ++k)
		if (gAsync[k].iSt && gAsync[k].iSt->Int() != KRequestPending)
			{
			RDebug::Printf("PH: proteus WS: async fn %x completed %d", gAsync[k].iFn, gAsync[k].iSt->Int());
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
	CheckAsync();
	}
static void BridgeSignal(void*, int aCount) { RThread t; while (aCount-- > 0) t.RequestSignal(); }

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
	RDebug::Printf("PH: proteus progress %u M instructions: pc %s, lr %s", (TUint)(aCpu.iSteps >> 20), pc, lr);
	}

// CArmCpu::iOnForeignPc: the guest called native code -- in practice a virtual function of a native object it shares, like the
// CBitmapFont in the font server's heap (CFbsFont::DoFontSpecInTwips calls Address()->FontSpecInTwips()), whose vtable points into
// the native fntstr.dll. The guest has the ARM twin of that DLL with the same class layouts, so run the twin's function on the
// object: the native function is export i of the native DLL -> export i of the ARM DLL; else slot k of an exported native vtable
// (_ZTV.., the export points at its first slot) -> slot k of the ARM vtable of the same ordinal. 0 = none (the fetch faults).
static uint32_t ForeignPc(proteus::CArmCpu& aCpu, uint32_t aPc, void* aHleP)
	{
	const proteus::THle& hle = *(const proteus::THle*)aHleP;
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
	// "-b" first: every ARM image through the image broker (the P2e test: what a program without AllFiles gets when PlatSec is enforced)
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
	TFileName path = arg.Length() ? arg : TFileName(KDefaultExe);
	RFs fs;
	User::LeaveIfError(fs.Connect());
	CleanupClosePushL(fs);
	TEntry euserEntry;
	// installed = the entry is there; a handler holding only the program's capabilities may not look into \sys\ (denied = there)
	const TInt euserState = fs.Entry(_L("z:\\sys\\bin\\arm\\euser.dll"), euserEntry);
	const TBool realEuser = forceReal || (!forceHle && arg.Length() && (euserState == KErrNone || euserState == KErrPermissionDenied));
	RDebug::Printf("PH: proteus euser: %s", realEuser ? "real ARM" : "HLE");
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
	// the guest region: its own local disconnected chunk, KGuestBytes reserved; the kernel hands out committed pages cleared
	RChunk chunk;
	User::LeaveIfError(chunk.CreateDisconnectedLocal(0, 0, KGuestBytes));
	CleanupClosePushL(chunk);
	TUint8* mem = chunk.Base();
	proteus::CArmCpu* cpu = new (ELeave) proteus::CArmCpu;
	cpu->iHost = mem;
	cpu->iBase = (TUint32)mem;
	cpu->iSize = KGuestBytes;
	cpu->iCommit = CommitGuest;
	cpu->iCommitCtx = &chunk;
	cpu->iPageMap = (TUint8*)User::AllocZL(KGuestBytes >> 12);
	CleanupStack::PushL(cpu->iPageMap);
	// the main thread's stack at the region top (proteusrt.cpp StartThreadStack: the thread data 4 KB below the top)
	if (!cpu->Commit(cpu->iBase + KGuestBytes - proteus::KStackBytes, proteus::KStackBytes))
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
		bridge->iUsed[i] = bridge->iConnected[i] = bridge->iPropUsed[i] = EFalse;
		bridge->iObjType[i] = 0;
		}
	proteus::TIpcBridge ipc = { bridge, BridgeCreate, BridgeSend, BridgeOwns, BridgeClose, BridgeShare, BridgeWaitAny, BridgeSignal, BridgeKernel };
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
	proteus::RunExe(*cpu, hle, 0xFFFFFFFFu);		// no instruction budget for a real app (it was 50 M: about.exe hit it while drawing text)
	RDebug::Printf("PH: proteus guest halted: %d exit %d after %d instructions, fault %x '%s'", (TInt)cpu->iHalt, cpu->iExitCode, (TInt)cpu->iSteps, cpu->iFaultAddr, cpu->iNote);
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
static TInt RunTimed(const TDesC& aName, const TDesC& aArgs, TInt aSeconds, TExitType& aExit, TInt& aReason)
	{
	aExit = EExitPending;
	aReason = 0;
	RProcess p;
	const TInt r = p.Create(aName, aArgs);
	if (r != KErrNone)
		return r;
	TRequestStatus st, tst;
	RTimer t;
	t.CreateLocal();
	p.Logon(st);
	p.Resume();
	t.After(tst, aSeconds * 1000000);
	User::WaitForRequest(st, tst);
	if (st == KRequestPending)
		{
		p.LogonCancel(st);
		User::WaitForRequest(st);
		}
	else
		{
		t.Cancel();
		User::WaitForRequest(tst);
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
			// real installer (sisinst = the silent install pheget uses), then the app started by its plain name like the menu does (the loader hook)
			CDir* sis = NULL;
			if (fs.GetDir(_L("c:\\sis\\*.sis"), KEntryAttNormal, ESortByName, sis) == KErrNone && sis->Count())
				{
				TExitType et = EExitKill;
				TInt reason = KErrNone, ir = KErrNone;
				for (TInt i = 0; i < sis->Count() && ir == KErrNone && et == EExitKill && reason == KErrNone; ++i)
					{
					TFileName pkg(_L("c:\\sis\\"));
					pkg.Append((*sis)[i].iName);
					ir = RunTimed(_L("sisinst.exe"), pkg, 600, et, reason);
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
					const TInt ar = RunTimed(exe, KNullDesC, 240, et, reason);
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

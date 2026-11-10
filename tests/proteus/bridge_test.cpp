// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// tests/proteus/bridge_test.cpp -- host test of the native-library bridge (rung R1, os/src/proteus/bridge.cpp). The ARM EXE proteusbridge.exe
// (euser only: descriptors, RArray without a comparator, User::Alloc/ReAlloc/Free) runs with bridge.cfg "native euser.dll": its euser
// imports do NOT run the HLE or an ARM euser but the "native" table below. On the host that table is a small fake euser written against the
// same ABI the x32 DLL has (the AAPCS arguments arrive as the SysV call the generated trampolines make: 32-bit words, a 64-bit value, a
// TChar by value, a TPtrC16 returned by value); on the board the table is the real native euser. Exit code 0 = every check of the EXE passed.
// Build/run: bash tests/proteus/run.sh --bridge <proteusbridge.exe>
#include "../../os/src/proteus/proteusrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <stdarg.h>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <sys/syscall.h>

using namespace proteus;
typedef uint32_t U;
static CArmCpu* gCpu;
static U gHeapTop;
static U gCalls;

static U R32(U a) { return gCpu->Read32(a); }
static void W32(U a, U v) { gCpu->Write32(a, v); }
static U R16(U a) { return gCpu->Read16(a); }
// descriptors (e32des16.h): iLength = type << 28 | length; types EBufC 0, EPtrC 1, EPtr 2, EBuf 3; the buffer: TBuf after iMaxLength (+8), TPtrC at +4 (iPtr), TPtr at +8
static U Len(U d) { return R32(d) & 0x0FFFFFFF; }
static U Type(U d) { return R32(d) >> 28; }
static U Ptr16(U d) { switch (Type(d)) { case 1: return R32(d + 4); case 2: return R32(d + 8); case 3: return d + 8; default: return d + 4; } }
static U Max(U d) { return Type(d) == 1 ? Len(d) : R32(d + 4); }
static void SetLen(U d, U n) { W32(d, (R32(d) & 0xF0000000) | n); }

// ---- the fake native euser: one function per export the EXE imports, by NAME (as the real binding does)
static U Fk_TBufBase16_C2(U self, U max) { W32(self, 3u << 28); W32(self + 4, max); return 0; }
static U Fk_TBufBase8_C2(U self, U max) { W32(self, 3u << 28); W32(self + 4, max); return 0; }
static U Fk_TPtrC16_C1_ptr(U self, U p) { U n = 0; while (R16(p + 2 * n)) ++n; W32(self, (1u << 28) | n); W32(self + 4, p); return 0; }
static U Fk_TPtrC16_C1_des(U self, U d) { W32(self, (1u << 28) | Len(d)); W32(self + 4, Ptr16(d)); return 0; }
static U Fk_TDes16_Copy(U self, U d) { U n = Len(d); if (n > Max(self)) return 0xBAD; for (U i = 0; i < n; ++i) gCpu->Write16(Ptr16(self) + 2 * i, (uint16_t)R16(Ptr16(d) + 2 * i)); SetLen(self, n); return 0; }
static U Fk_TDes8_Copy(U self, U d) { U n = Len(d); for (U i = 0; i < n; ++i) gCpu->Write8(self + 8 + i, (uint8_t)R16(Ptr16(d) + 2 * i)); SetLen(self, n); return 0; }
static U Fk_TDes16_Append(U self, U d) { U n = Len(self), m = Len(d); if (n + m > Max(self)) return 0xBAD; for (U i = 0; i < m; ++i) gCpu->Write16(Ptr16(self) + 2 * (n + i), (uint16_t)R16(Ptr16(d) + 2 * i)); SetLen(self, n + m); return 0; }
static U Fk_TDes16_Zero(U self) { SetLen(self, 0); return 0; }
static U Fk_TDes16_UpperCase(U self) { for (U i = 0; i < Len(self); ++i) { U c = R16(Ptr16(self) + 2 * i); if (c >= 'a' && c <= 'z') gCpu->Write16(Ptr16(self) + 2 * i, (uint16_t)(c - 32)); } return 0; }
static void AppendNum(U self, int64_t v) { char b[24]; snprintf(b, sizeof b, "%lld", (long long)v); U n = Len(self); for (char* p = b; *p; ++p) gCpu->Write16(Ptr16(self) + 2 * n++, (uint16_t)*p); SetLen(self, n); }
static U Fk_TDes16_Num(U self, uint64_t v) { SetLen(self, 0); AppendNum(self, (int64_t)v); return 0; }	// (self, a 64-bit value in an even register pair)
static U Fk_TDes16_AppendNum(U self, uint64_t v) { AppendNum(self, (int64_t)v); return 0; }
static int Cmp(U a, U b) { U na = Len(a), nb = Len(b); for (U i = 0; i < na && i < nb; ++i) { int d = (int)R16(Ptr16(a) + 2 * i) - (int)R16(Ptr16(b) + 2 * i); if (d) return d; } return (int)na - (int)nb; }
static U Fk_TDesC16_Compare(U self, U o) { return (U)Cmp(self, o); }
static U Fk_TDesC16_Find(U self, U o) { U n = Len(self), m = Len(o); for (U i = 0; i + m <= n; ++i) { U j = 0; while (j < m && R16(Ptr16(self) + 2 * (i + j)) == R16(Ptr16(o) + 2 * j)) ++j; if (j == m) return i; } return (U)-1; }
struct TChar { U c; };
struct TPtrC16V { U len, ptr; };
static U Fk_TDesC16_Locate(U self, TChar c) { for (U i = 0; i < Len(self); ++i) if (R16(Ptr16(self) + 2 * i) == c.c) return i; return (U)-1; }
static U Fk_TDesC16_LocateReverse(U self, TChar c) { for (U i = Len(self); i-- > 0;) if (R16(Ptr16(self) + 2 * i) == c.c) return i; return (U)-1; }
static TPtrC16V Fk_TDesC16_Left(U self, U n) { TPtrC16V v; v.len = (1u << 28) | (n < Len(self) ? n : Len(self)); v.ptr = Ptr16(self); return v; }
static U Fk_TDesC8_AtC(U self, U i) { return self + 8 + i; }
// RPointerArrayBase: iCount, iEntries, iAllocated, iGranularity (e32cmn.h)
static U Fk_RPAB_C2(U self) { W32(self, 0); W32(self + 4, 0); W32(self + 8, 0); W32(self + 12, 8); return 0; }
static std::mutex gHeapLock;
static U Alloc(U n) { std::lock_guard<std::mutex> l(gHeapLock); if (n > (16u << 20)) return 0; U a = gHeapTop; gHeapTop += (n + 15) & ~15u; return a; }
static void Grow(U self) { U cnt = R32(self), ent = R32(self + 4), cap = R32(self + 8); if (cnt < cap) return; U ncap = cap ? cap * 2 : 8, ne = Alloc(4 * ncap); for (U i = 0; i < cnt; ++i) W32(ne + 4 * i, R32(ent + 4 * i)); W32(self + 4, ne); W32(self + 8, ncap); }
static U Fk_RPAB_Append(U self, U p) { Grow(self); U c = R32(self); W32(R32(self + 4) + 4 * c, p); W32(self, c + 1); return 0; }
static U Fk_RPAB_Insert(U self, U p, U at) { Grow(self); U c = R32(self), e = R32(self + 4); for (U i = c; i > at; --i) W32(e + 4 * i, R32(e + 4 * (i - 1))); W32(e + 4 * at, p); W32(self, c + 1); return 0; }
static U Fk_RPAB_Remove(U self, U at) { U c = R32(self), e = R32(self + 4); for (U i = at; i + 1 < c; ++i) W32(e + 4 * i, R32(e + 4 * (i + 1))); W32(self, c - 1); return 0; }
static U Fk_RPAB_Close(U self) { W32(self, 0); W32(self + 4, 0); W32(self + 8, 0); return 0; }
static U Fk_RPAB_At(U self, U i) { return R32(self + 4) + 4 * i; }
static U Fk_RPAB_Count(U self) { return R32(self); }
static U Fk_User_Alloc(U n) { return Alloc(n); }
static U Fk_User_ReAlloc(U p, U n, U) { U q = Alloc(n); for (U i = 0; i < n; ++i) gCpu->Write8(q + i, gCpu->Read8(p + i)); return q; }
static U Fk_User_Free(U) { return 0; }
static U Fk_memset(U p, U c, U n) { for (U i = 0; i < n; ++i) gCpu->Write8(p + i, (uint8_t)c); return p; }
static U Fk_TBufBase16_C2_des(U self, U d, U max) { W32(self, 3u << 28); W32(self + 4, max); return Fk_TDes16_Copy(self, d); }
static U Fk_TPtrC8_C1_ptr(U self, U p) { U n = 0; while (gCpu->Read8(p + n)) ++n; W32(self, (1u << 28) | n); W32(self + 4, p); return 0; }
// RArrayBase: iCount, iEntries, iEntrySize, iAllocated, iGranularity ... (e32cmn.h): inline elements of iEntrySize bytes
static U Fk_RAB_C2(U self, U esz) { W32(self, 0); W32(self + 4, 0); W32(self + 8, esz); W32(self + 12, 0); return 0; }
static U Fk_RAB_Append(U self, U p) { U c = R32(self), esz = R32(self + 8), cap = R32(self + 12); if (c == cap) { U ncap = cap ? cap * 2 : 8, ne = Alloc(esz * ncap); for (U i = 0; i < c * esz; ++i) gCpu->Write8(ne + i, gCpu->Read8(R32(self + 4) + i)); W32(self + 4, ne); W32(self + 12, ncap); }
	for (U i = 0; i < esz; ++i) gCpu->Write8(R32(self + 4) + c * esz + i, gCpu->Read8(p + i)); W32(self, c + 1); return 0; }
static U Fk_RAB_At(U self, U i) { return R32(self + 4) + i * R32(self + 8); }
static U Fk_RAB_Close(U self) { W32(self, 0); W32(self + 4, 0); W32(self + 12, 0); return 0; }
// HeapSort(order): the native side calls the guest comparator through the reverse stub the bridge made for it
static U Fk_RAB_HeapSort(U self, U order)
{
	typedef U (*Cmp)(U, U, U, U);
	Cmp cmp = (Cmp)BridgeStubFn(order);
	U n = R32(self), esz = R32(self + 8), base = R32(self + 4);
	for (U i = 1; i < n; ++i)
		for (U j = i; j > 0 && (int)cmp(base + (j - 1) * esz, base + j * esz, 0, 0) > 0; --j)
			for (U k = 0; k < esz; ++k) { uint8_t t = gCpu->Read8(base + (j - 1) * esz + k); gCpu->Write8(base + (j - 1) * esz + k, gCpu->Read8(base + j * esz + k)); gCpu->Write8(base + j * esz + k, t); }
	return 0;
}
static U Fk_nop() { return 0; }
static U Fk_memcpy(U d, U sr, U n) { for (U i = 0; i < n; ++i) gCpu->Write8(d + i, gCpu->Read8(sr + i)); return d; }
static U Fk_memmove(U d, U sr, U n) { std::vector<uint8_t> t(n); for (U i = 0; i < n; ++i) t[i] = gCpu->Read8(sr + i); for (U i = 0; i < n; ++i) gCpu->Write8(d + i, t[i]); return d; }
static U Fk_memcmp(U a, U b, U n) { for (U i = 0; i < n; ++i) { int x = gCpu->Read8(a + i) - gCpu->Read8(b + i); if (x) return (U)x; } return 0; }
static U Fk_RomHeader() { static U h; if (!h) { h = Alloc(256); for (U i = 0; i < 256; ++i) gCpu->Write8(h + i, 0); } return h; }
// a fake native cleanup stack: items {fn, ptr} and level marks; TCleanupItem arrives by value (its function already a reverse stub)
struct TClean { U iFn, iPtr; bool iMark; };
static std::vector<TClean> gClean;
struct TItem { U fn, ptr; };
static U Fk_PushL_item(TItem i) { gClean.push_back({ i.fn, i.ptr, false }); return 0; }
static void DestroyTop() { TClean c = gClean.back(); gClean.pop_back(); typedef U (*F)(U, U, U, U); ((F)BridgeStubFn(c.iFn))(c.iPtr, 0, 0, 0); }
static U Fk_PopAndDestroyN(U n) { while (n--) DestroyTop(); return 0; }
static U Fk_Mark() { gClean.push_back({ 0, 0, true }); return 1; }
static U Fk_UnMark(U) { if (!gClean.empty() && gClean.back().iMark) gClean.pop_back(); return 0; }
static void TrapLeave(uint32_t, int32_t) { while (!gClean.empty() && !gClean.back().iMark) DestroyTop(); if (!gClean.empty()) gClean.pop_back(); }
// the native leave of the fake: a host C++ throw, caught by the fake scheduler's "TRAP" around RunL (which then calls RunError)
static U Fk_Leave(U r) { throw (int)r; }
extern U gSchedVt;
static U Fk_TrapCleanupNew() { U o = Alloc(16); W32(o, gSchedVt + 8); return o; }
static U Fk_strlen(U a) { U n = 0; while (gCpu->Read8(a + n)) ++n; return n; }
static U Fk_strcmp(U a, U b) { for (U i = 0;; ++i) { int x = gCpu->Read8(a + i) - gCpu->Read8(b + i); if (x || !gCpu->Read8(a + i)) return (U)x; } }
// ---- R1 threads: RThread::Create starts a REAL host thread at Resume; its entry is the reverse stub the bridge made for the guest's ThreadFn,
// which runs on a new guest CPU (bridge.cpp RunOnNewCpu)
struct TFakeThread { U iFn, iArg, iStatus; int iExit; std::thread iT; };
static std::vector<TFakeThread*> gThreads;
static U Fk_RThread_Create(U self, U name, U fn, U stack, U heap, U arg, U owner)
{
	TFakeThread* t = new TFakeThread(); t->iFn = fn; t->iArg = arg; t->iStatus = 0; t->iExit = -1;
	gThreads.push_back(t); W32(self, (U)gThreads.size()); return 0;
}
static TFakeThread* Th(U self) { U h = R32(self); return h && h <= gThreads.size() ? gThreads[h - 1] : 0; }
static U Fk_RThread_Logon(U self, U st) { W32(st, 0x80000001u); Th(self)->iStatus = st; return 0; }
static U Fk_RThread_Resume(U self)
{
	TFakeThread* t = Th(self);
	t->iT = std::thread([t]() {
		typedef U (*F)(U, U, U, U);
		t->iExit = (int)((F)BridgeStubFn(t->iFn))(t->iArg, 0, 0, 0);
		printf("  native thread %ld: guest thread function returned %d\n", (long)syscall(SYS_gettid), t->iExit);
	});
	return 0;
}
static U Fk_User_WaitForRequest(U st)
{
	for (TFakeThread* t : gThreads) if (t->iStatus == st && t->iT.joinable()) { t->iT.join(); W32(st, (U)t->iExit); }
	return 0;
}
static U Fk_RThread_ExitReason(U self) { return (U)Th(self)->iExit; }
static U Fk_RHandleBase_Close(U self) { W32(self, 0); return 0; }
// ---- R1 active objects: a native CActiveScheduler that calls RunL through the object's vtable (patched to reverse stubs by the bridge), and a
// natively constructed scheduler whose own vtable is NATIVE (its destructor is a native address the guest calls: BridgeNativeCall)
static std::vector<U> gActive;
U gSchedVt; static U gStop, gNativeDtorCalls;
static U Fk_CActive_C2(U self, U pri) { W32(self + 4, 0); W32(self + 8, 0); W32(self + 20, pri); return self; }
static U Fk_CActive_D2(U self) { for (size_t i = 0; i < gActive.size(); ++i) if (gActive[i] == self) gActive.erase(gActive.begin() + i); return self; }
static U Fk_CActive_SetActive(U self) { W32(self + 8, R32(self + 8) | 1); return 0; }
static U Fk_CActive_Cancel(U self) { W32(self + 8, R32(self + 8) & ~1u); return 0; }
static U Fk_Sched_Add(U a) { gActive.push_back(a); return 0; }
static U Fk_Sched_Install(U) { return 0; }
static U Fk_Sched_Stop() { gStop = 1; return 0; }
static U Fk_Sched_Start()
{
	gStop = 0;
	for (int guard = 0; !gStop && guard < 100; ++guard)
		for (U a : std::vector<U>(gActive))
			if ((R32(a + 8) & 1) && R32(a + 4) != 0x80000001u)
			{
				W32(a + 8, R32(a + 8) & ~1u);
				const U runl = R32(R32(a) + 16);			// slot 4: RunL
				if ((runl & 0xFF000000u) != 0xE0000000u) { printf("  RunL slot %x is not a reverse stub: the vtable was not patched\n", runl); return 0; }
				typedef U (*F)(U, U, U, U);
				try { ((F)BridgeStubFn(runl))(a, 0, 0, 0); }
				catch (int err)
				{
					const U runerr = R32(R32(a) + 20);	// slot 5: RunError
					printf("  native scheduler: RunL left %d -> RunError\n", err);
					if ((int)((F)BridgeStubFn(runerr))(a, (U)err, 0, 0) != 0) return 0;
				}
				break;
			}
	return 0;
}
static U Fk_Sched_D0(U) { ++gNativeDtorCalls; return 0; }
static U Fk_Sched_C2(U self) { W32(self, gSchedVt + 8); return self; }
static U Fk_RequestComplete(U pp, U r) { W32(R32(pp), r); W32(pp, 0); return 0; }
static U Fk_User_AllocZ(U n) { U a = Alloc(n); for (U i = 0; i < n; ++i) gCpu->Write8(a + i, 0); return a; }
// TDes::(Append)FormatList against the x32/SysV va_list the bridge builds (here the host's LP64 __va_list_tag, same layout): the conversions are
// done by libc's snprintf for the numbers and by hand for %S (a guest descriptor pointer) -- a wrong slot layout gives a wrong string
static void FormatInto(U self, U fmtd, void* va, bool append, bool wide)
{
	va_list ap; memcpy(ap, va, sizeof(ap));
	auto chars = [&](U d, std::string& out) { U p = Type(d) == 1 ? R32(d + 4) : Type(d) == 2 ? R32(d + 8) : Type(d) == 3 ? d + 8 : d + 4; for (U i = 0; i < Len(d); ++i) out += (char)(wide ? R16(p + 2 * i) : gCpu->Read8(p + i)); };
	std::string f, out; chars(fmtd, f);
	for (size_t i = 0; i < f.size(); ++i)
	{
		if (f[i] != '%') { out += f[i]; continue; }
		std::string piece = "%"; ++i;
		while (i < f.size() && strchr("-+0 #", f[i])) piece += f[i++];
		if (f[i] == '*') { piece += std::to_string(va_arg(ap, int)); ++i; } else while (isdigit((unsigned char)f[i])) piece += f[i++];
		if (f[i] == '.') { piece += f[i++]; if (f[i] == '*') { piece += std::to_string(va_arg(ap, int)); ++i; } else while (isdigit((unsigned char)f[i])) piece += f[i++]; }
		bool big = false; while (f[i] == 'l' || f[i] == 'L') { big |= f[i] == 'L'; ++i; }
		char buf[80]; const char c = f[i];
		if (strchr("diuxXoc", c)) { piece += big ? std::string("ll") + c : std::string(1, c); if (big) snprintf(buf, sizeof buf, piece.c_str(), va_arg(ap, long long)); else snprintf(buf, sizeof buf, piece.c_str(), va_arg(ap, int)); out += buf; }
		else if (strchr("efgEG", c)) { piece += c; snprintf(buf, sizeof buf, piece.c_str(), va_arg(ap, double)); out += buf; }
		else if (c == 'S') { U d = (U)(uintptr_t)va_arg(ap, void*); chars(d, out); }
		else if (c == '%') out += '%';
	}
	U n = append ? Len(self) : 0, base = wide ? Ptr16(self) : self + 8;
	for (char ch : out) { if (wide) gCpu->Write16(base + 2 * n, (uint8_t)ch); else gCpu->Write8(base + n, (uint8_t)ch); ++n; }
	SetLen(self, n);
}
static void Fk_FormatList16(U self, U f, void* va, U) { FormatInto(self, f, va, false, true); }
static void Fk_AppendFormatList16(U self, U f, void* va, U) { FormatInto(self, f, va, true, true); }
static void Fk_FormatList8(U self, U f, void* va, U) { FormatInto(self, f, va, false, false); }
static void Fk_AppendFormatList8(U self, U f, void* va, U) { FormatInto(self, f, va, true, false); }

struct TFake { const char* iName; void* iFn; };
static const TFake KFakes[] = {
	{ "_ZN10TBufBase16C2Ei", (void*)Fk_TBufBase16_C2 }, { "_ZN9TBufBase8C2Ei", (void*)Fk_TBufBase8_C2 },
	{ "_ZN7TPtrC16C1EPKt", (void*)Fk_TPtrC16_C1_ptr }, { "_ZN7TPtrC16C1ERK7TDesC16", (void*)Fk_TPtrC16_C1_des },
	{ "_ZN6TDes164CopyERK7TDesC16", (void*)Fk_TDes16_Copy }, { "_ZN5TDes84CopyERK7TDesC16", (void*)Fk_TDes8_Copy },
	{ "_ZN6TDes166AppendERK7TDesC16", (void*)Fk_TDes16_Append }, { "_ZN6TDes164ZeroEv", (void*)Fk_TDes16_Zero },
	{ "_ZN6TDes169UpperCaseEv", (void*)Fk_TDes16_UpperCase }, { "_ZN6TDes163NumEx", (void*)Fk_TDes16_Num },
	{ "_ZN6TDes169AppendNumEx", (void*)Fk_TDes16_AppendNum }, { "_ZNK7TDesC167CompareERKS_", (void*)Fk_TDesC16_Compare },
	{ "_ZNK7TDesC164FindERKS_", (void*)Fk_TDesC16_Find }, { "_ZNK7TDesC166LocateE5TChar", (void*)Fk_TDesC16_Locate },
	{ "_ZNK7TDesC1613LocateReverseE5TChar", (void*)Fk_TDesC16_LocateReverse }, { "_ZNK7TDesC164LeftEi", (void*)Fk_TDesC16_Left },
	{ "_ZNK6TDesC83AtCEi", (void*)Fk_TDesC8_AtC }, { "_ZN17RPointerArrayBaseC2Ev", (void*)Fk_RPAB_C2 },
	{ "_ZN17RPointerArrayBase6AppendEPKv", (void*)Fk_RPAB_Append }, { "_ZN17RPointerArrayBase6InsertEPKvi", (void*)Fk_RPAB_Insert },
	{ "_ZN17RPointerArrayBase6RemoveEi", (void*)Fk_RPAB_Remove }, { "_ZN17RPointerArrayBase5CloseEv", (void*)Fk_RPAB_Close },
	{ "_ZNK17RPointerArrayBase2AtEi", (void*)Fk_RPAB_At }, { "_ZNK17RPointerArrayBase5CountEv", (void*)Fk_RPAB_Count },
	{ "_ZN4User5AllocEi", (void*)Fk_User_Alloc }, { "_ZN4User7ReAllocEPvii", (void*)Fk_User_ReAlloc }, { "_ZN4User4FreeEPv", (void*)Fk_User_Free },
	{ "_ZN6TDes1610FormatListERK7TDesC16St9__va_list", (void*)Fk_FormatList16 }, { "_ZN6TDes1616AppendFormatListERK7TDesC16St9__va_listP14TDes16Overflow", (void*)Fk_AppendFormatList16 },
	{ "_ZN5TDes810FormatListERK6TDesC8St9__va_list", (void*)Fk_FormatList8 }, { "_ZN5TDes816AppendFormatListERK6TDesC8St9__va_listP13TDes8Overflow", (void*)Fk_AppendFormatList8 },
	{ "_ZN6TDes166FormatE11TRefByValueIK7TDesC16Ez", (void*)Fk_nop }, { "_ZN6TDes1612AppendFormatE11TRefByValueIK7TDesC16Ez", (void*)Fk_nop },
	{ "_ZN5TDes86FormatE11TRefByValueIK6TDesC8Ez", (void*)Fk_nop },		// (the variadic rows' own natives: resolved, never called -- the handler calls FormatList)
	{ "_ZN10TBufBase16C2ERK7TDesC16i", (void*)Fk_TBufBase16_C2_des },
	{ "_ZN6TPtrC8C1EPKh", (void*)Fk_TPtrC8_C1_ptr },
	{ "_ZN10RArrayBaseC2Ei", (void*)Fk_RAB_C2 }, { "_ZN10RArrayBase6AppendEPKv", (void*)Fk_RAB_Append }, { "_ZNK10RArrayBase2AtEi", (void*)Fk_RAB_At },
	{ "_ZN10RArrayBase5CloseEv", (void*)Fk_RAB_Close }, { "_ZN10RArrayBase8HeapSortEPFiPKvS1_E", (void*)Fk_RAB_HeapSort },
	{ "_ZN7RThread6CreateERK7TDesC16PFiPvEiP10RAllocatorS3_10TOwnerType", (void*)Fk_RThread_Create }, { "_ZNK7RThread5LogonER14TRequestStatus", (void*)Fk_RThread_Logon },
	{ "_ZNK7RThread6ResumeEv", (void*)Fk_RThread_Resume }, { "_ZN4User14WaitForRequestER14TRequestStatus", (void*)Fk_User_WaitForRequest },
	{ "_ZNK7RThread10ExitReasonEv", (void*)Fk_RThread_ExitReason }, { "_ZN11RHandleBase5CloseEv", (void*)Fk_RHandleBase_Close },
	{ "_ZN7CActiveC2Ei", (void*)Fk_CActive_C2 }, { "_ZN7CActiveD2Ev", (void*)Fk_CActive_D2 }, { "_ZN7CActive9SetActiveEv", (void*)Fk_CActive_SetActive },
	{ "_ZN7CActive6CancelEv", (void*)Fk_CActive_Cancel }, { "_ZN16CActiveScheduler3AddEP7CActive", (void*)Fk_Sched_Add },
	{ "_ZN16CActiveScheduler7InstallEPS_", (void*)Fk_Sched_Install }, { "_ZN16CActiveScheduler4StopEv", (void*)Fk_Sched_Stop },
	{ "_ZN16CActiveScheduler5StartEv", (void*)Fk_Sched_Start }, { "_ZN16CActiveSchedulerC1Ev", (void*)Fk_Sched_C2 }, { "_ZN16CActiveSchedulerC2Ev", (void*)Fk_Sched_C2 },
	{ "_ZN4User15RequestCompleteERP14TRequestStatusi", (void*)Fk_RequestComplete }, { "_ZN4User6AllocZEi", (void*)Fk_User_AllocZ },
	{ "memset", (void*)Fk_memset }, { "memcpy", (void*)Fk_memcpy }, { "memmove", (void*)Fk_memmove }, { "memcmp", (void*)Fk_memcmp }, { "strlen", (void*)Fk_strlen }, { "strcmp", (void*)Fk_strcmp }, { "_ZN7UserSvr16RomHeaderAddressEv", (void*)Fk_RomHeader },
	{ "_ZN4User16MarkCleanupStackEv", (void*)Fk_Mark }, { "_ZN4User18UnMarkCleanupStackEP12TTrapHandler", (void*)Fk_UnMark },
	{ "_ZN12CleanupStack5PushLE12TCleanupItem", (void*)Fk_PushL_item }, { "_ZN12CleanupStack13PopAndDestroyEi", (void*)Fk_PopAndDestroyN }, { "_ZNK15XLeaveException9GetReasonEv", (void*)Fk_nop }, { "_ZN4User5LeaveEi", (void*)Fk_Leave }, { "_ZN12CTrapCleanup3NewEv", (void*)Fk_TrapCleanupNew },
	{ "_ZN4User12LeaveIfErrorEi", (void*)Fk_nop }, { "_ZN7UserSvr6DllTlsEii", (void*)Fk_nop }, { "_ZN7UserSvr9DllSetTlsEiiPv", (void*)Fk_nop },
	{ "_ZN7UserSvr19ExceptionDescriptorEm", (void*)Fk_nop }, { "_ZN7UserSvr6DllTlsEi", (void*)Fk_nop }, { "_ZN7UserSvr9DllSetTlsEiPv", (void*)Fk_nop }, { "_ZN4User15HandleExceptionEPv", (void*)Fk_nop }, { "_ZN4User9InvariantEv", (void*)Fk_nop },
};
static std::vector<void*> gFnTable;		// host: iNativeExport returns a 32-bit number, so the (64-bit) function pointers are handed out as indices

static uint32_t NativeExport(void*, const char* aDll, int aOrd)
{
	const char* n = BridgeNativeName(aDll, (uint32_t)aOrd);
	for (const TFake& f : KFakes)
		if (!strcmp(f.iName, n)) { ++gCalls; gFnTable.push_back(f.iFn); return (uint32_t)gFnTable.size(); }
	printf("  no fake for native %s ordinal %d (%s)\n", aDll, aOrd, n);
	return 0;
}
static uintptr_t Resolve(uint32_t t) { if (t >= 0x200000u && t < 0x300000u) t = (t - 0x200000u) >> 1;	// an even token (a vtable slot the guest branches to: bit 0 would be Thumb)
	if (!t || t > gFnTable.size()) { printf("  foreign pc %x is no native function\n", t); return 0; } return (uintptr_t)gFnTable[t - 1]; }
static void Log(const char* l, void*) { printf("  | %s\n", l); }
// the ARM runtime DLLs of a device-model EXE (drtaeabi.dll ...) from the EXE's own directory
static std::string gExeDir;
static std::vector<std::vector<uint8_t>> gDllImages;
static const uint8_t* FindDll(const char* aName, uint32_t& aLen, char& aDrive, void*)
{
	std::string n(aName);
	const size_t b = n.find_first_of("{["), e = n.rfind('.');
	if (b != std::string::npos && e != std::string::npos && e > b) n = n.substr(0, b) + n.substr(e);
	FILE* f = fopen((gExeDir + "/" + n).c_str(), "rb");
	if (!f) return 0;
	std::vector<uint8_t> v; uint8_t buf[4096]; size_t k;
	while ((k = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + k);
	fclose(f);
	gDllImages.push_back(v);
	aLen = (uint32_t)gDllImages.back().size(); aDrive = 'z';
	return gDllImages.back().data();
}

int main(int argc, char** argv)
{
	setvbuf(stdout, 0, _IONBF, 0);
	const char* path = argc > 1 ? argv[1] : "emitter/proteusbridge.exe";
	gExeDir = path; gExeDir = gExeDir.substr(0, gExeDir.rfind('/') == std::string::npos ? 0 : gExeDir.rfind('/'));
	FILE* f = fopen(path, "rb");
	if (!f) { printf("cannot open %s\n", path); return 2; }
	std::vector<uint8_t> img; uint8_t b[4096]; size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) img.insert(img.end(), b, b + n);
	fclose(f);
	CArmCpu cpu; gCpu = &cpu;
	std::vector<uint8_t> mem(32 << 20);
	cpu.iHost = mem.data(); cpu.iBase = 0x00400000; cpu.iSize = (uint32_t)mem.size();
	gHeapTop = cpu.iBase + (16 << 20);
	static TProcess proc;
	proc.iRealEuser = false;		// the gate: no ARM euser and no real-euser executive
	BridgeSetResolver(Resolve);
	BridgeSetLookup([](void*, const char*, int o) -> uint32_t { return 0xC0DE0000u | (uint32_t)o; }, 0);
	BridgeConfigure(getenv("BRTRACE") ? "native euser.dll\ntrace\n" : "# the euser imports go native\nnative euser.dll\n");
	{	// DATA imports: a vtable export (euser ordinal 1359 _ZTV5CBase) is the native address itself; a function export is not data
		uint32_t a = 0;
		const bool vt = BridgeDataImport("euser{000a0000}[100039e5].dll", 1359, a) && a == (0xC0DE0000u | 1359);
		const bool fn = !BridgeDataImport("euser.dll", 646, a);		// User::Alloc
		const bool other = !BridgeDataImport("efsrv.dll", 1359, a);	// efsrv is not native in this configuration
		printf("DATA imports: vtable %s, function stays a trap %s, other DLL %s\n", vt ? "ok" : "BAD", fn ? "ok" : "BAD", other ? "ok" : "BAD");
		if (!(vt && fn && other)) return 1;
	}
	if (!LoadProcess(cpu, img.data(), (uint32_t)img.size(), cpu.iBase + 0x1000, FindDll, 0, proc)) { printf("LOAD FAILED: %s\n", proc.iError); return 1; }
	TIpcBridge ipc; memset(&ipc, 0, sizeof ipc); ipc.iNativeExport = NativeExport;
	THle hle = { &proc.iMod[0], &proc, Log, 0, 0 };
	hle.iIpc = &ipc;
	{	// R1 threads: host thread ids, guest stacks from the fake heap, the interpreter on every CPU
		TBridgeThreads thr; memset(&thr, 0, sizeof thr);
		thr.iThreadId = []() -> uint32_t { return (uint32_t)syscall(SYS_gettid); };
		thr.iAlloc = [](uint32_t n) -> uint32_t { return ::Alloc(n); };
		thr.iTrapLeave = TrapLeave;
		thr.iLog = [](const char* l) { printf("  | %s%c", l, 10); };
		BridgeSetThreads(thr);
	}
	{	// the native CActiveScheduler's vtable: [offset, typeinfo, D1, D0, ...] in guest memory, slot D0 = a native function (a token gResolve maps)
		gFnTable.push_back((void*)Fk_Sched_D0);
		gSchedVt = Alloc(64);
		for (U k = 0; k < 16; ++k) W32(gSchedVt + 4 * k, 0);
		W32(gSchedVt + 8 + 4, 0x200000u + 2 * (U)gFnTable.size());	}
	cpu.iOnForeignPc = BridgeOnForeignPc;
	RunExe(cpu, hle, 50000000);
	printf("native destructor calls from ARM code: %u%c", gNativeDtorCalls, 10);
	printf("halt %d exit %d steps %llu note '%s' natives bound %u\n", (int)cpu.iHalt, cpu.iExitCode, (unsigned long long)cpu.iSteps, cpu.iNote, gCalls);
	const bool ok = cpu.iHalt == EExited && cpu.iExitCode == 0 && gCalls >= 20 && gNativeDtorCalls >= 1;
	printf("bridge: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

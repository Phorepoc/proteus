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
	c.Run(~0ull);
	const uint32_t result = c.R[0];
	const bool ok = c.iHalt == EReturned;
	c.iJit = jit; c.iSliceSteps = slice; c.iNextSlice = next;
	if (ok)											// back to the interrupted guest code, exactly as it was
	{
		memcpy(c.R, R, sizeof(R)); memcpy(c.iD, D, sizeof(D));
		c.iN = flags[0]; c.iZ = flags[1]; c.iC = flags[2]; c.iV = flags[3]; c.iT = flags[4]; c.iGE = flags[5];
		c.iFpscr = fpscr; c.iHalt = halt; c.iExitCode = code;
	}
	return ok ? result : 0;						// (anything else: the guest stopped in the callback; the halt stays and ends the run)
}

namespace
{
enum { KMaxStubs = 64 };
struct TRevStub { uint32_t iFn; };
TRevStub gStub[KMaxStubs];
int gStubCount;
CArmCpu* gStubCpu;
template <int K> uint32_t RevStub(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return BridgeCallGuest(*gStubCpu, gStub[K].iFn, a, b, c, d); }
typedef uint32_t (*TStubFn)(uint32_t, uint32_t, uint32_t, uint32_t);
template <int K> struct TStubTable { static void Fill(TStubFn* t) { t[K] = &RevStub<K>; TStubTable<K - 1>::Fill(t); } };
template <> struct TStubTable<-1> { static void Fill(TStubFn*) {} };
TStubFn gStubFn[KMaxStubs];
}

// the native address of stub k: on x32 the function itself; on an LP64 host test a token BridgeStubFn turns back into the function
static uint32_t StubAddress(int k) { return sizeof(void*) == 4 ? (uint32_t)(uintptr_t)gStubFn[k] : 0xE0000000u | (uint32_t)k; }
uintptr_t BridgeStubFn(uint32_t aAddr) { return (aAddr & 0xFF000000u) == 0xE0000000u ? (uintptr_t)gStubFn[aAddr & 0xFFFFFF] : (uintptr_t)aAddr; }

static uint32_t BrCb(CArmCpu& aCpu, uint32_t aGuestFn)
{
	if (!aGuestFn || !aCpu.InRegion(aGuestFn & ~1u, 2))
		return aGuestFn;
	if (!gStubFn[0]) TStubTable<KMaxStubs - 1>::Fill(gStubFn);
	gStubCpu = &aCpu;
	for (int k = 0; k < gStubCount; ++k)
		if (gStub[k].iFn == aGuestFn) return StubAddress(k);
	if (gStubCount == KMaxStubs) { aCpu.Halt(EHleStop); strncpy(aCpu.iNote, "bridge: out of callback stubs", sizeof(aCpu.iNote) - 1); return 0; }
	gStub[gStubCount].iFn = aGuestFn;
	return StubAddress(gStubCount++);
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
};
// imports that are not bridged even for a native DLL: the guest EXE's start-up and exit belong to Proteus, not to the native process
const char* const KKeepHle[] = { "_ZN4User11InitProcessEv", "_ZN7UserSvr9DllSetTlsEiiPv", "_ZN4User4ExitEi", "_ZN6RDebug6PrintfEPKcz" };
TBridgeDll gDll[2] = { { "euser.dll", KBridge_euser, 0, false, 0, 0 }, { "efsrv.dll", KBridge_efsrv, 0, false, 0, 0 } };
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
	if (d->iKeepHle[row])
		return false;
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

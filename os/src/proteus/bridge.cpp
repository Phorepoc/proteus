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

bool BridgeTrap(CArmCpu& aCpu, THle& aHle, const char* aDll, uint32_t aOrd)
{
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
	if (!d->iFn[row])
	{
		if (e.iNative && aHle.iIpc && aHle.iIpc->iNativeExport)
			d->iFn[row] = aHle.iIpc->iNativeExport(aHle.iIpc->iCtx, d->iName, e.iNative);
		if (d->iFn[row] && gResolve)
			d->iFn[row] = gResolve((uint32_t)d->iFn[row]);
		if (!d->iFn[row])
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

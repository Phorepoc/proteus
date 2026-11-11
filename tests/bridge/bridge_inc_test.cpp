// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// Compiles the generated bridge tables (os/src/proteus/gen/bridge_*.inc) against a stand-in CPU and AAPCS argument cursor
// and calls a few trampolines. Nothing in Proteus includes the tables yet (rung R0). Build: tests/bridge/run.sh
#include <stdint.h>
extern "C" void* memcpy(void*, const void*, unsigned long);

struct CArmCpu { uint32_t R[16]; uint32_t Stack[16]; uint8_t* HostPtr(uint32_t a) { return (uint8_t*)(uintptr_t)a; } };

// the AAPCS cursor: r0-r3 then the stack; a 64-bit value takes an even register pair or an 8-aligned stack slot
struct BrArgs
{
	CArmCpu& c; int n;
	explicit BrArgs(CArmCpu& aCpu) : c(aCpu), n(0) {}
	uint32_t Raw(int i) { return i < 4 ? c.R[i] : c.Stack[i - 4]; }
	uint32_t W() { return Raw(n++); }
	uint64_t L() { if (n & 1) ++n; uint64_t v = Raw(n) | (uint64_t)Raw(n + 1) << 32; n += 2; return v; }
	double D() { uint64_t b = L(); double d; memcpy(&d, &b, 8); return d; }
	float F() { uint32_t b = W(); float f; memcpy(&f, &b, 4); return f; }
	void S(void* aDst, unsigned aSize, unsigned aAlign)
	{
		if (aAlign > 4 && (n & 1)) ++n;
		for (unsigned i = 0; i < aSize; i += 4) { uint32_t w = Raw(n++); memcpy((char*)aDst + i, &w, aSize - i < 4 ? aSize - i : 4); }
	}
};

// the hand-written handlers the tables name (bridge.cpp has them; here stand-ins so the tables link)
#define BR_STUB(n) void n(CArmCpu&, uintptr_t) {}
BR_STUB(BrOvr_Format16) BR_STUB(BrOvr_AppendFormat16) BR_STUB(BrOvr_AppendFormatOvf16) BR_STUB(BrOvr_FormatList16) BR_STUB(BrOvr_AppendFormatList16)
BR_STUB(BrOvr_Format8) BR_STUB(BrOvr_AppendFormat8) BR_STUB(BrOvr_AppendFormatOvf8) BR_STUB(BrOvr_FormatList8) BR_STUB(BrOvr_AppendFormatList8)
BR_STUB(BrOvr_ProcessFileName) BR_STUB(BrOvr_ProcessType) BR_STUB(BrOvr_CommandLine) BR_STUB(BrOvr_CommandLineLength) BR_STUB(BrOvr_ExeExportData)
// Open C's libc (round 16)
#define BR_LIBC(n) BR_STUB(BrOvr_libc_##n)
BR_LIBC(printf) BR_LIBC(fprintf) BR_LIBC(sprintf) BR_LIBC(snprintf) BR_LIBC(asprintf) BR_LIBC(scanf) BR_LIBC(fscanf) BR_LIBC(sscanf)
BR_LIBC(wprintf) BR_LIBC(fwprintf) BR_LIBC(swprintf) BR_LIBC(wscanf) BR_LIBC(fwscanf) BR_LIBC(swscanf) BR_LIBC(vprintf) BR_LIBC(vfprintf)
BR_LIBC(vsprintf) BR_LIBC(vsnprintf) BR_LIBC(vasprintf) BR_LIBC(vscanf) BR_LIBC(vfscanf) BR_LIBC(__vfscanf) BR_LIBC(vsscanf) BR_LIBC(vwprintf)
BR_LIBC(vfwprintf) BR_LIBC(vswprintf) BR_LIBC(vwscanf) BR_LIBC(vfwscanf) BR_LIBC(vswscanf) BR_LIBC(Words3) BR_LIBC(Words4) BR_LIBC(exit)
BR_LIBC(_exit) BR_LIBC(abort) BR_LIBC(setjmp) BR_LIBC(longjmp)

static uint32_t BrCb(CArmCpu&, uint32_t a) { return a; }
static void BrCbField(CArmCpu&, uint8_t*) {}
#include "../../os/src/proteus/gen/bridge_shapes.inc"
#include "../../os/src/proteus/gen/bridge_euser.inc"
#include "../../os/src/proteus/gen/bridge_efsrv.inc"
#include "../../os/src/proteus/gen/bridge_libc.inc"
#include "../../os/src/proteus/gen/bridge_libpthread.inc"

static uint32_t Add3(uint32_t a, uint32_t b, uint32_t c) { return a + b + c; }
// (the trampolines cast a 32-bit guest address to a function: running one needs an x32 process, so this test only
// checks that every trampoline compiles and that the tables are complete)

int main()
{
	// every ordinal row is present, in ordinal order, and each has a trampoline, a flag that explains why not, or is data
	const TBridgeExport* tabs[4] = { KBridge_euser, KBridge_efsrv, KBridge_libc, KBridge_libpthread };
	const uint32_t counts[4] = { KBridge_euser_Count, KBridge_efsrv_Count, KBridge_libc_Count, KBridge_libpthread_Count };
	for (int t = 0; t < 4; ++t)
		for (uint32_t i = 0; i < counts[t]; ++i)
		{
			if (tabs[t][i].iOrdinal != i + 1) return 1;
			if (!tabs[t][i].iCall && !(tabs[t][i].iFlags & (BR_DATA | BR_VTABLE | BR_TYPEINFO | BR_ABSENT | BR_VARIADIC | BR_STRUCTFP | BR_UNRESOLVED | BR_PMF | BR_OVERRIDE)))
				return 2;
		}
	(void)Add3;
	return KBridgeShapeCount > 0 ? 0 : 3;
}

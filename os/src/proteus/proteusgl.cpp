// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusgl.cpp -- an ARM app's OpenGL ES / EGL calls run on the NATIVE Khronos DLLs (libEGL, libGLESv2, libGLESv1_CM,
// libGLES_CM): no ARM GL is staged, the imports of those DLLs stay with Proteus and each import slot calls the native
// export of the same ordinal (the frozen .def tables; gen/glthunks.inc, made by tools/gen_gl_thunks.py, has one entry
// per import with its signature). The Symbian EABI is soft-float: every argument is a 32-bit word in r0-r3 then the
// stack, a float result comes back in r0; on x32 a guest pointer is the host address, so pointers pass as they are.
// What the native side returns as a pointer lies outside the guest region and is mapped (GlRetPtr): strings are copied
// into guest memory, a function from eglGetProcAddress becomes a guest-callable slot of the same thunk table.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "armcpu.h"
#include "proteusrt.h"

namespace proteus
{

struct TGlThunk
{
	const char* iDll;
	uint16_t iOrd;
	const char* iName;
	void (*iCall)(CArmCpu& aCpu, uint32_t aFn, const char* aName);
};

static inline uint32_t GlArg(CArmCpu& aCpu, int aN)
{
	return aN < 4 ? aCpu.R[aN] : aCpu.Read32(aCpu.R[13] + 4 * (aN - 4));
}
static inline float GlArgF(CArmCpu& aCpu, int aN)
{
	const uint32_t w = GlArg(aCpu, aN);
	float f;
	memcpy(&f, &w, 4);
	return f;
}
static inline void GlRetF(CArmCpu& aCpu, float aF)
{
	memcpy(&aCpu.R[0], &aF, 4);
}
static uint32_t GlRetPtr(CArmCpu& aCpu, const char* aName, uint32_t aPtr);

#include "gen/glthunks.inc"

static const uint32_t KThunkCount = sizeof(KGlThunks) / sizeof(KGlThunks[0]);
static const uint32_t KGlModule = 0x3FF;		// an import slot's module field: a thunk of this table (eglGetProcAddress)
static uint32_t gNative[KThunkCount];			// the native function per thunk (0 = not looked up yet)
static uint32_t gProcSlot[KThunkCount];			// eglGetProcAddress: the guest address made for a thunk (0 = none yet)
static THle* gHle;								// the guest being served (one native thread runs all guest threads)

// the DLL name of an import without its version {...} and UID [...] decorations, compared case-insensitively
static bool SameDll(const char* aImport, const char* aDll)
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

bool IsGlDll(const char* aImport)
{
	return SameDll(aImport, "libEGL.dll") || SameDll(aImport, "libGLESv2.dll") || SameDll(aImport, "libGLESv1_CM.dll")
		|| SameDll(aImport, "libGLES_CM.dll");
}

// strings the native side returns (glGetString, eglQueryString): one guest copy per native string, kept for the process
// (the API promises the pointer stays valid)
static uint32_t GuestString(CArmCpu& aCpu, uint32_t aNative)
{
	static uint32_t native[64], guest[64];
	static int count;
	for (int i = 0; i < count; ++i)
		if (native[i] == aNative) return guest[i];
	if (count == 64) return 0;
	const char* s = (const char*)(uintptr_t)aNative;
	const uint32_t g = GuestBytes(aCpu, gHle->iExec, s, (uint32_t)strlen(s) + 1);
	if (!g) return 0;
	native[count] = aNative;
	guest[count++] = g;
	return g;
}

// eglGetProcAddress(name): the function of that name in the thunk table, as a guest-callable import slot (NULL for a name
// it does not have -- what EGL itself returns for an extension it does not support: there is no signature to call it with)
static uint32_t ProcSlot(CArmCpu& aCpu, const char* aName)
{
	static const char* const order[] = { "libGLESv2.dll", "libGLESv1_CM.dll", "libEGL.dll", "libGLES_CM.dll" };
	TProcess* proc = gHle->iProcess;
	for (int o = 0; o < 4 && proc; ++o)
		for (uint32_t i = 0; i < KThunkCount; ++i)
		{
			if (strcmp(KGlThunks[i].iDll, order[o]) || strcmp(KGlThunks[i].iName, aName)) continue;
			if (gProcSlot[i]) return gProcSlot[i];
			if (proc->iSlotCount == proc->iSlotMax)
			{
				const uint32_t nmax = proc->iSlotMax ? proc->iSlotMax * 2 : 1024;
				uint32_t* ns = (uint32_t*)realloc(proc->iSlots, nmax * sizeof(uint32_t));
				if (!ns) return 0;
				proc->iSlots = ns;
				proc->iSlotMax = nmax;
			}
			const uint32_t slot = proc->iSlotCount++;
			proc->iSlots[slot] = (KGlModule << 22) | i;
			return gProcSlot[i] = KTrapBase | (slot << 1);
		}
	Say(*gHle, "[proteus] eglGetProcAddress(%s): not in the Khronos tables -> NULL", aName);
	return 0;
}

static uint32_t GlRetPtr(CArmCpu& aCpu, const char* aName, uint32_t aPtr)
{
	if (!strcmp(aName, "eglGetProcAddress"))
		return ProcSlot(aCpu, (const char*)(uintptr_t)aCpu.R[0]);		// (r0 = the name: the result is stored after this returns)
	return aPtr ? GuestString(aCpu, aPtr) : 0;							// glGetString, eglQueryString
}

// where the time of a frame goes: the native GL calls, the native eglSwapBuffers (the present: symgl's bitmap copy + WSERV), and the rest
// (the interpreter, the other native services). Each call is timed with the kernel's fast counter. That counter is 32 bits and wraps in seconds, so
// it is extended to 64 bits at every sample (gaps between GL calls are far shorter than a wrap) and the log line scales the sums by the wall
// time (the tick count): no counter frequency is needed (the HAL's figure is not the PC's actual counter rate).
static uint32_t gSwaps, gLastTick, gLastSwaps, gPrevFc;
static uint64_t gWallFc, gSwapFc, gGlFc, gLastWallFc, gLastSwapFc, gLastGlFc;
static uint32_t Counter(CArmCpu& aCpu, THle& aHle, const char* aName)
{
	uint32_t r[4] = {0, 0, 0, 0};
	if (aHle.iIpc && aHle.iIpc->iKernel) aHle.iIpc->iKernel(aHle.iIpc->iCtx, aName, r, aCpu);
	return r[0];
}
static uint32_t Sample(CArmCpu& aCpu, THle& aHle)		// the counter now; the extended wall total moves with it
{
	const uint32_t fc = Counter(aCpu, aHle, "EFastExecFastCounter");
	gWallFc += (uint32_t)(fc - gPrevFc);
	gPrevFc = fc;
	return fc;
}
static void Stats(CArmCpu& aCpu, THle& aHle)
{
	const uint32_t tick = Counter(aCpu, aHle, "EFastExecNTickCount");
	const uint64_t dWall = gWallFc - gLastWallFc;
	if (gLastTick && dWall)
	{
		const uint32_t dtMs = tick - gLastTick, dSw = gSwaps - gLastSwaps;
		if (dtMs && dSw)
		{
			// tenths of a millisecond per frame
			const uint64_t frT = (uint64_t)dtMs * 10 / dSw, fps10 = (uint64_t)dSw * 10000 / dtMs;
			const uint64_t glT = frT * (gGlFc - gLastGlFc) / dWall, swT = frT * (gSwapFc - gLastSwapFc) / dWall;
			const int64_t rest = (int64_t)frT - (int64_t)glT - (int64_t)swT;
			char line[240];
			snprintf(line, sizeof(line), "[proteus] GLSTAT frames %u: %u.%u fps over %u ms; per frame %u.%u ms = GL calls %u.%u + present %u.%u + rest (interpreter, services) %d.%u",
				gSwaps, (unsigned)(fps10 / 10), (unsigned)(fps10 % 10), dtMs, (unsigned)(frT / 10), (unsigned)(frT % 10), (unsigned)(glT / 10), (unsigned)(glT % 10),
				(unsigned)(swT / 10), (unsigned)(swT % 10), (int)(rest / 10), (unsigned)((rest < 0 ? -rest : rest) % 10));
			Say(aHle, "%s", line);
		}
	}
	gLastTick = tick; gLastSwaps = gSwaps; gLastSwapFc = gSwapFc; gLastGlFc = gGlFc; gLastWallFc = gWallFc;
}

static bool Call(CArmCpu& aCpu, THle& aHle, uint32_t aIndex)
{
	const TGlThunk& t = KGlThunks[aIndex];
	if (!gNative[aIndex])
		gNative[aIndex] = aHle.iIpc && aHle.iIpc->iNativeExport ? aHle.iIpc->iNativeExport(aHle.iIpc->iCtx, t.iDll, t.iOrd) : 0;
	if (!gNative[aIndex])
	{
		char note[120];
		snprintf(note, sizeof(note), "native %s has no ordinal %u (%s)", t.iDll, t.iOrd, t.iName);
		Say(aHle, "[proteus] %s", note);
		strncpy(aCpu.iNote, note, sizeof(aCpu.iNote) - 1);
		aCpu.Halt(EHleStop);
		return true;
	}
	if (!strcmp(t.iName, "eglCreateWindowSurface"))
		return GlWindowSurface(aCpu, aHle, gNative[aIndex]);
	gHle = &aHle;
	// the trace: the first calls of each function (arguments, result), then every 60th eglSwapBuffers; eglCreate* always
	static uint32_t seen[KThunkCount];
	const uint32_t a0 = aCpu.R[0], a1 = aCpu.R[1], a2 = aCpu.R[2], a3 = aCpu.R[3];
	const bool swap = !strncmp(t.iName, "eglSwap", 7);
	const uint64_t us0 = aHle.iIpc && aHle.iIpc->iNowUs ? aHle.iIpc->iNowUs(aHle.iIpc->iCtx) : 0;
	const uint32_t fc0 = Sample(aCpu, aHle);
	t.iCall(aCpu, gNative[aIndex], t.iName);
	const uint32_t fc1 = Sample(aCpu, aHle);
	// A native call is one indivisible step for the interpreter: while it ran, no other guest thread could, and the native replies other threads
	// wait for (the audio stream's buffer requests) were not looked at. A call that took long ends the time slice now, so the scheduler polls
	// the kernel and runs a thread that became ready at the next instruction instead of at the end of the slice.
	if (us0 && aHle.iIpc->iNowUs(aHle.iIpc->iCtx) - us0 > 300 && aCpu.iSliceSteps) aCpu.iNextSlice = aCpu.iSteps;
	(swap ? gSwapFc : gGlFc) += (uint32_t)(fc1 - fc0);
	if (swap && ++gSwaps % 30 == 0) Stats(aCpu, aHle);
	const uint32_t n = ++seen[aIndex];
	if (n <= 2 || (!strncmp(t.iName, "eglSwap", 7) && n % 60 == 0) || !strncmp(t.iName, "eglCreate", 9))
	{
		char line[200];
		snprintf(line, sizeof(line), "[proteus] GL %s #%u (%x,%x,%x,%x) -> %x", t.iName, n, a0, a1, a2, a3, aCpu.R[0]);
		Say(aHle, "%s", line);
	}
	aCpu.SetReturn();
	return true;
}

bool GlTrap(CArmCpu& aCpu, THle& aHle, const char* aDll, uint32_t aOrd)
{
	for (uint32_t i = 0; i < KThunkCount; ++i)
		if (KGlThunks[i].iOrd == aOrd && SameDll(aDll, KGlThunks[i].iDll))
			return Call(aCpu, aHle, i);
	return false;
}

bool GlTrapSlot(CArmCpu& aCpu, THle& aHle, uint32_t aSlotValue)
{
	if ((aSlotValue >> 22) != KGlModule || (aSlotValue & 0xFFFF) >= KThunkCount) return false;
	return Call(aCpu, aHle, aSlotValue & 0xFFFF);
}

} // namespace proteus

// proteusexec.cpp -- OURS (Proteus P2): the executive (SWI) side of a guest that runs the REAL ARM euser.dll (our 2010-source device build,
// emitter/euser_dev.dll). euser's Exec:: stubs are `swi n` (slow) / `swi 0x800000|n` (fast), arguments r0-r3, result r0
// (baseline u32exec.h, numbers in toolchain/generated/exec_enum.h -> exec_names.inc). This is a user-mode stand-in for the kernel of ONE
// single-threaded process: thread-local executive state (heap, trap handler, active scheduler, TLS), chunks carved from the top of the guest
// region, handles as small integers, RDebug output to the log. Unknown calls stop the guest with their name (the next thing to emulate).
#include "proteusrt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace proteus
{
#include "exec_names.inc"

enum { KFast = 0x800000 };
enum { KErrNone = 0, KErrNotFound = -1, KErrNotSupported = -5, KErrNoMemory = -4, KErrArgument = -6, KErrBadHandle = -8 };

static uint32_t& Slot(TExecState& e, uint32_t aKey)
{
	for (int i = 0; i < e.iTlsCount; ++i) if (e.iTlsKey[i] == aKey) return e.iTlsVal[i];
	static uint32_t dummy;
	if (e.iTlsCount >= (int)(sizeof(e.iTlsKey) / sizeof(e.iTlsKey[0]))) return dummy = 0;
	e.iTlsKey[e.iTlsCount] = aKey;
	e.iTlsVal[e.iTlsCount] = 0;
	return e.iTlsVal[e.iTlsCount++];
}

// a chunk = [base, base + max) carved downward from the guest region's top (below the main stack); its pages are committed by CommitPages (host pages too: CArmCpu::Commit)
static int NewChunk(CArmCpu& cpu, TExecState& e, uint32_t aMax)
{
	aMax = (aMax + 0xFFF) & ~0xFFFu;
	if (e.iChunkTop == 0) e.iChunkTop = (cpu.iBase + cpu.iSize - KStackBytes) & ~0xFFFu;
	if (e.iChunkCount >= (int)(sizeof(e.iChunkBase) / sizeof(e.iChunkBase[0])) || e.iChunkTop - aMax < e.iLowWater) return KErrNoMemory;
	e.iChunkTop -= aMax;
	e.iChunkBase[e.iChunkCount] = e.iChunkTop;
	e.iChunkMax[e.iChunkCount] = aMax;
	e.iChunkPages[e.iChunkCount] = (uint8_t*)calloc(aMax >> 12 ? aMax >> 12 : 1, 1);
	e.iChunkAtt[e.iChunkCount] = 0;
	return KHandleChunk0 + e.iChunkCount++;
}

// the committed amount of a chunk (RChunk::Size()) and the commit / decommit primitives of the kernel's DChunk, page granular.
// Freshly committed pages read as zero (the kernel hands out cleared pages); decommitted ones are cleared too.
static uint32_t Committed(TExecState& e, int i)
{
	uint32_t n = 0;
	for (uint32_t p = 0; p < (e.iChunkMax[i] >> 12); ++p) n += e.iChunkPages[i][p];
	return n << 12;
}

static void ClearPages(CArmCpu& cpu, TExecState& e, int i, uint32_t p0, uint32_t p1)
{
	const uint32_t a = e.iChunkBase[i] + (p0 << 12), n = (p1 - p0) << 12;
	if (cpu.InRegion(a, n)) memset(cpu.iHost + (a - cpu.iBase), 0, n);		// (profile: a Write32 loop was 2.6% of a heap-heavy run)
	else for (uint32_t x = a; x < a + n; x += 4) cpu.Write32(x, 0);
}

static int CommitPages(CArmCpu& cpu, TExecState& e, int i, uint32_t aOffset, uint32_t aSize)
{
	if ((aOffset & 0xFFF) || aOffset > e.iChunkMax[i] || aSize > e.iChunkMax[i] - aOffset) return KErrArgument;
	const uint32_t p0 = aOffset >> 12, p1 = (aOffset + aSize + 0xFFF) >> 12;
	for (uint32_t p = p0; p < p1; ++p) if (e.iChunkPages[i][p]) return -11;		// KErrAlreadyExists
	if (!cpu.Commit(e.iChunkBase[i] + (p0 << 12), (p1 - p0) << 12)) return KErrNoMemory;
	for (uint32_t p = p0; p < p1; ++p) e.iChunkPages[i][p] = 1;
	ClearPages(cpu, e, i, p0, p1);
	return KErrNone;
}

static int DecommitPages(CArmCpu& cpu, TExecState& e, int i, uint32_t aOffset, uint32_t aSize)
{
	if (aOffset > e.iChunkMax[i] || aSize > e.iChunkMax[i] - aOffset) return KErrArgument;
	const uint32_t p0 = aOffset >> 12, p1 = (aOffset + aSize + 0xFFF) >> 12;
	for (uint32_t p = p0; p < p1; ++p)
		if (e.iChunkPages[i][p]) { e.iChunkPages[i][p] = 0; ClearPages(cpu, e, i, p, p + 1); cpu.Commit(e.iChunkBase[i] + (p << 12), 0x1000, false); }
	return KErrNone;			// pages that were not committed are ignored
}

// normal / double-ended chunks: the committed region is [bottom, top)
static void Region(TExecState& e, int i, uint32_t& aBottom, uint32_t& aTop)
{
	const uint32_t n = e.iChunkMax[i] >> 12;
	uint32_t p = 0;
	while (p < n && !e.iChunkPages[i][p]) ++p;
	aBottom = p << 12;
	while (p < n && e.iChunkPages[i][p]) ++p;
	aTop = p << 12;
}

static int ChunkIndex(TExecState& e, uint32_t aHandle)
{
	const int i = (int)(aHandle & 0xFFFF) - (KHandleChunk0 & 0xFFFF);
	return (aHandle & 0xFFFF0000u) == (KHandleChunk0 & 0xFFFF0000u) && i >= 0 && i < e.iChunkCount ? i : -1;
}

// a TDesC8/TDesC16 in guest memory -> UTF-8-ish bytes (16-bit: low byte)
static int GuestDes(CArmCpu& cpu, uint32_t aDes, char* aOut, int aMax)
{
	const uint32_t w = cpu.Read32(aDes);
	const uint32_t type = w >> 28, len = w & 0x0FFFFFFF;
	uint32_t ptr = 0;
	switch (type)
	{
	case 0: ptr = aDes + 4; break;			// EBufC
	case 1: ptr = cpu.Read32(aDes + 4); break;	// EPtrC
	case 2: ptr = cpu.Read32(aDes + 8); break;	// EPtr
	case 3: ptr = aDes + 8; break;			// EBuf
	case 4: ptr = cpu.Read32(aDes + 8) + 4; break;	// EBufCPtr
	default: return -1;
	}
	int n = 0;
	for (uint32_t i = 0; i < len && n < aMax - 1; ++i) aOut[n++] = (char)cpu.Read8(ptr + i);
	aOut[n] = 0;
	return n;
}

// write a C string into a guest TDes8 (EBuf: [len|type][max][data], EPtr: [len|type][max][ptr]); truncated to the max length
static void SetDes8(CArmCpu& cpu, uint32_t aDes, const char* aText)
{
	const uint32_t w = cpu.Read32(aDes), type = w >> 28, max = cpu.Read32(aDes + 4);
	const uint32_t data = type == 2 ? cpu.Read32(aDes + 8) : aDes + 8;
	uint32_t n = 0;
	for (; aText[n] && n < max; ++n) cpu.Write8(data + n, (uint8_t)aText[n]);
	cpu.Write32(aDes, (type << 28) | n);
}

// lr, then the code addresses found on the stack (a heuristic backtrace), as module+offset (+ nearest export) lines in the log
void Backtrace(CArmCpu& cpu, THle& h)
{
	if (!h.iProcess || !h.iLog) return;
	char w[120], l[160];
	Where(*h.iProcess, cpu, cpu.R[14], w, sizeof(w));
	snprintf(l, sizeof(l), "[proteus]   lr %s", w);
	h.iLog(l, h.iLogCtx);
	int shown = 0;
	for (uint32_t a = cpu.R[13]; shown < 40 && a < cpu.R[13] + 0x2000 && cpu.Valid(a, 4); a += 4)
	{
		const uint32_t v = cpu.Read32(a) & ~1u;
		for (int m = 0; m < h.iProcess->iCount; ++m)
		{
			const TLoaded& md = h.iProcess->iMod[m];
			if (v > md.iLoadAddr && v < md.iEnd && (!md.iExportDir || v < md.iExportDir))		// in the code, before the export table
			{
				Where(*h.iProcess, cpu, v, w, sizeof(w));
				snprintf(l, sizeof(l), "[proteus]   stack %s", w);
				h.iLog(l, h.iLogCtx);
				++shown;
				break;
			}
		}
	}
}

bool ExecCall(CArmCpu& cpu, THle& h, uint32_t imm)
{
	TExecState& e = h.iExec;
	const bool fast = (imm & KFast) != 0;
	const uint32_t n = imm & 0x7FFFFF;
	const char* name = fast ? (n < sizeof(KFastExecNames) / sizeof(KFastExecNames[0]) ? KFastExecNames[n] : "?")
							: (n < sizeof(KSlowExecNames) / sizeof(KSlowExecNames[0]) ? KSlowExecNames[n] : "?");
	uint32_t* r = cpu.R;
	++e.iCalls;
	// trace: the first 400 slow execs (and every WaitForAnyRequest among them) with the caller -- where a guest that went quiet blocks
	static int traced = 0;
	if (h.iLog && h.iProcess && traced < 400 && (!fast || !strcmp(name, "EFastExecWaitForAnyRequest")))
	{
		++traced;
		char w[120], l[200];
		Where(*h.iProcess, cpu, r[14], w, sizeof(w));
		snprintf(l, sizeof(l), "[proteus] exec %s r0 %x r1 %x lr %s", name, r[0], r[1], w);
		h.iLog(l, h.iLogCtx);
		if (fast) Backtrace(cpu, h);
	}
	if (fast)
	{
		if (!strcmp(name, "EFastExecWaitForAnyRequest"))
		{
			if (!h.iIpc) { strncpy(cpu.iNote, "WaitForAnyRequest with nothing to wait for", sizeof(cpu.iNote) - 1); cpu.Halt(EHleStop); return true; }
			h.iIpc->iWaitAny(h.iIpc->iCtx);
			return true;
		}
		if (!strcmp(name, "EFastExecHeap")) { r[0] = e.iHeap; return true; }
		if (!strcmp(name, "EFastExecRomHeaderAddress")) { r[0] = h.iRomHeader; return true; }
		if ((!strcmp(name, "EFastExecGetGlobalUserData") || !strcmp(name, "EFastExecRomRootDirectoryAddress") || !strcmp(name, "EFastExecUTCOffset")
			|| !strcmp(name, "EFastExecNTickCount") || !strcmp(name, "EFastExecFastCounter")) && h.iIpc && h.iIpc->iKernel && h.iIpc->iKernel(h.iIpc->iCtx, name, r, cpu)) return true;	// native value (locale data in the ROM window)
		if (!strcmp(name, "EFastExecHeapSwitch"))
		{
			const uint32_t o = e.iHeap;
			e.iHeap = r[0];
			if (h.iLog) { char l[80]; snprintf(l, sizeof(l), "[proteus] HeapSwitch %x -> %x", o, r[0]); h.iLog(l, h.iLogCtx); }
			r[0] = o;
			return true;
		}
		if (!strcmp(name, "EFastExecTrapHandler")) { r[0] = e.iTrapHandler; return true; }
		if (!strcmp(name, "EFastExecSetTrapHandler")) { const uint32_t o = e.iTrapHandler; e.iTrapHandler = r[0]; r[0] = o; return true; }
		if (!strcmp(name, "EFastExecActiveScheduler")) { r[0] = e.iActiveScheduler; return true; }
		if (!strcmp(name, "EFastExecSetActiveScheduler")) { e.iActiveScheduler = r[0]; return true; }
		if (!strcmp(name, "EFastExecPushTrapFrame")) { cpu.Write32(r[0] + 0x48, e.iTrapHandler); cpu.Write32(r[0] + 0x40, e.iTrapFrame); e.iTrapFrame = r[0]; r[0] = e.iTrapHandler; return true; }	// sexec.cpp: TTrap::iHandler, iNext; returns the handler
		if (!strcmp(name, "EFastExecPopTrapFrame")) { const uint32_t f = e.iTrapFrame; if (f) e.iTrapFrame = cpu.Read32(f + 0x40); r[0] = f; return true; }
		if (!strcmp(name, "EFastExecDebugMask") || !strcmp(name, "EFastExecDebugMaskIndex")) { r[0] = 0; return true; }
		if (!strcmp(name, "EFastExecKernelConfigFlags")) { r[0] = 0; return true; }
		if (!strcmp(name, "EFastExecFastCounter") || !strcmp(name, "EFastExecNTickCount")) { r[0] = (uint32_t)cpu.iSteps; return true; }
		if (!strcmp(name, "EFastExecLockPeriod") || !strcmp(name, "EFastExecUTCOffset")) { r[0] = 0; return true; }
		if (!strcmp(name, "EFastExecCheckFilter")) { r[0] = 0; return true; }
		if (!strcmp(name, "EFastExecSetReentryPoint")) { return true; }
		if (!strcmp(name, "EFastExecFastAtomicAdd32")) { const uint32_t o = cpu.Read32(r[0]); cpu.Write32(r[0], o + r[1]); r[0] = o; return true; }
		if (!strcmp(name, "EFastExecFastAtomicCas32"))
		{
			const uint32_t cur = cpu.Read32(r[0]), q = cpu.Read32(r[1]);
			if (cur == q) { cpu.Write32(r[0], r[2]); r[0] = 1; } else { cpu.Write32(r[1], cur); r[0] = 0; }
			return true;
		}
		if (!strcmp(name, "EFastExecFastAtomicTas32")) { const uint32_t o = cpu.Read32(r[0]); cpu.Write32(r[0], (int32_t)o >= (int32_t)r[1] ? o + r[2] : o + r[3]); r[0] = o; return true; }
		if (!strcmp(name, "EFastExecFastAtomicAxo32")) { const uint32_t o = cpu.Read32(r[0]); cpu.Write32(r[0], (o & r[1]) ^ r[2]); r[0] = o; return true; }
		if (!strcmp(name, "EFastExecFastAtomicTau32")) { const uint32_t o = cpu.Read32(r[0]); cpu.Write32(r[0], o >= r[1] ? o + r[2] : o + r[3]); r[0] = o; return true; }
	}
	else
	{
		if (h.iIpc && h.iIpc->iKernel && (!strncmp(name, "EExecProperty", 13) || !strcmp(name, "EExecOpenObject") || !strcmp(name, "EExecObjectNext") || !strcmp(name, "EExecPlatSecDiagnostic") || !strncmp(name, "EExecChunk", 10)
			|| !strncmp(name, "EExecMutex", 10) || !strncmp(name, "EExecSemaphore", 14) || !strncmp(name, "EExecChangeNotifier", 19) || !strncmp(name, "EExecTimer", 10) || !strncmp(name, "EExecTimeNow", 12) || !strcmp(name, "EExecTickCount") || !strcmp(name, "EExecHandleClose"))
			&& h.iIpc->iKernel(h.iIpc->iCtx, name, r, cpu))
			return true;
		if (!strcmp(name, "EExecDebugPrint"))
		{
			char buf[256];
			if (GuestDes(cpu, r[0], buf, sizeof(buf)) >= 0 && h.iLog) h.iLog(buf, h.iLogCtx);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecDllTls")) { r[0] = Slot(e, r[0]); return true; }		// (handle, dllUid) -> keyed by the handle
		if (!strcmp(name, "EExecDllSetTls")) { Slot(e, r[0]) = r[2]; r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecDllFreeTls")) { Slot(e, r[0]) = 0; return true; }
		if (!strcmp(name, "EExecChunkBase")) { const int i = ChunkIndex(e, r[0]); r[0] = i >= 0 ? e.iChunkBase[i] : 0; return true; }
		if (!strcmp(name, "EExecChunkMaxSize")) { const int i = ChunkIndex(e, r[0]); r[0] = i >= 0 ? e.iChunkMax[i] : 0; return true; }
		if (!strcmp(name, "EExecChunkSize")) { const int i = ChunkIndex(e, r[0]); r[0] = i >= 0 ? Committed(e, i) : 0; return true; }
		if (!strcmp(name, "EExecChunkBottom") || !strcmp(name, "EExecChunkTop"))
		{
			const int i = ChunkIndex(e, r[0]);
			uint32_t b = 0, t = 0;
			if (i >= 0) Region(e, i, b, t);
			r[0] = !strcmp(name, "EExecChunkBottom") ? b : t;
			return true;
		}
		if (!strcmp(name, "EExecChunkAdjust"))	// (handle, type, a1, a2): commits / decommits pages (CommitPages, DecommitPages)
		{
			// RChunk adjust types (e32std.h TAdjust): 0 Adjust, 1 AdjustDoubleEnded, 2 Commit(offset, size), 3 Decommit(offset, size), 4 Allocate.
			// The memory is always there, but the real kernel hands out freshly committed pages ZEROED, and the hybrid heap (RHybridHeap:
			// slab + page allocator below the heap object) relies on that -- stale data from a decommitted page reused as a fresh slab page
			// handed a live block out twice (CAknButton's RArray over the toolbar item, 2026-10-17). So commit/decommit clear the range.
			const int ci = ChunkIndex(e, r[0]);
			if (ci < 0) { r[0] = (uint32_t)KErrBadHandle; return true; }
			uint32_t bot = 0, top = 0;
			Region(e, ci, bot, top);
			int res = KErrNone;
			switch (r[1])
			{
			case 0:		// Adjust(newSize): normal chunk, committed [0, newSize)
			{
				const uint32_t ns = (r[2] + 0xFFF) & ~0xFFFu;
				if (ns > e.iChunkMax[ci]) { res = KErrArgument; break; }
				if (ns > top) res = CommitPages(cpu, e, ci, top, ns - top);
				else if (ns < top) res = DecommitPages(cpu, e, ci, ns, top - ns);
				break;
			}
			case 1:		// AdjustDoubleEnded(bottom, top)
			{
				const uint32_t nb = r[2] & ~0xFFFu, nt = (r[3] + 0xFFF) & ~0xFFFu;
				if (nb > nt || nt > e.iChunkMax[ci]) { res = KErrArgument; break; }
				DecommitPages(cpu, e, ci, 0, e.iChunkMax[ci]);			// (simple: re-commit the new region; contents of a double-ended heap move)
				res = nt > nb ? CommitPages(cpu, e, ci, nb, nt - nb) : KErrNone;
				break;
			}
			case 2: res = CommitPages(cpu, e, ci, r[2], r[3]); break;
			case 3: res = DecommitPages(cpu, e, ci, r[2], r[3]); break;
			case 4:		// Allocate(size): find a free run, commit it, return its offset
			{
				const uint32_t np = (r[2] + 0xFFF) >> 12, n = e.iChunkMax[ci] >> 12;
				res = KErrNoMemory;
				for (uint32_t p = 0; p + np <= n; ++p)
				{
					uint32_t k = 0;
					while (k < np && !e.iChunkPages[ci][p + k]) ++k;
					if (k == np) { CommitPages(cpu, e, ci, p << 12, np << 12); res = (int)(p << 12); break; }
					p += k;
				}
				break;
			}
			default: break;		// lock / unlock: nothing to do
			}
			r[0] = (uint32_t)res;
			return true;
		}
		if (!strcmp(name, "EExecChunkCreate"))
		{
			// TInt ChunkCreate(TOwnerType, const TDesC8* name, TChunkCreate& info): info = iAtt, iForceFixed, iInitialBottom, iInitialTop, iMaxSize ...
			const uint32_t info = r[2];
			const int hnd = NewChunk(cpu, e, cpu.Read32(info + 16));
			if (hnd > 0)		// the initial committed region: [iInitialBottom, iInitialTop)
			{
				const int nc = ChunkIndex(e, (uint32_t)hnd);
				e.iChunkAtt[nc] = cpu.Read32(info);
				const uint32_t ib = cpu.Read32(info + 8) & ~0xFFFu, it = (cpu.Read32(info + 12) + 0xFFF) & ~0xFFFu;
				if (it > ib && it <= e.iChunkMax[nc]) CommitPages(cpu, e, nc, ib, it - ib);
			}
			if (h.iLog)
			{
				char l[160];
				const int ci = ChunkIndex(e, (uint32_t)hnd);
				snprintf(l, sizeof(l), "[proteus] ChunkCreate att %x bottom %x top %x max %x -> handle %x base %x", cpu.Read32(info), cpu.Read32(info + 8),
					cpu.Read32(info + 12), cpu.Read32(info + 16), hnd, ci >= 0 ? e.iChunkBase[ci] : 0);
				h.iLog(l, h.iLogCtx);
			}
			r[0] = (uint32_t)hnd;
			return true;
		}
		TIpcBridge* ipc = h.iIpc;
		if (!strcmp(name, "EExecHandleClose") && ipc && ipc->iOwns(ipc->iCtx, r[0])) { r[0] = ipc->iClose(ipc->iCtx, r[0]); return true; }
		if (!strcmp(name, "EExecHandleClose") || !strcmp(name, "EExecHandleDuplicate")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecMathRandom"))		// Math::Random: 32 random bits (xorshift32, seeded once from the clock and an address)
		{
			static uint32_t s = 0;
			if (!s) s = (uint32_t)time(0) ^ (uint32_t)(uintptr_t)&s ^ 0x9E3779B9u;
			s ^= s << 13; s ^= s >> 17; s ^= s << 5;
			r[0] = s;
			return true;
		}
		// ---- the guest's loader: RLibrary::Load = a "!Loader" session (ELoadLibrary) + the DLL lock / attach / lookup execs, all served
		// here against the guest's module list -- the native loader would load x86 code ----
		if (!strcmp(name, "EExecWaitDllLock") || !strcmp(name, "EExecReleaseDllLock") || !strcmp(name, "EExecLibraryAttached")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecLibraryAttach"))		// (handle, TInt& numEps, TLinAddr* eps): the new modules' entry points, dependencies first
		{
			const int mod = (int)(r[0] & 0xFFFF);
			const int maxEps = (int)cpu.Read32(r[1]);
			int n = 0;
			if ((r[0] & 0xFFFF0000u) == KHandleLib0 && h.iProcess && mod < h.iProcess->iCount)
				for (int m = h.iProcess->iCount - 1; m >= e.iLibFirstNew && n < maxEps; --m)
					if (CallsEntry(cpu, h.iProcess->iMod[m])) cpu.Write32(r[2] + 4 * n++, h.iProcess->iMod[m].iEntry);
			e.iLibFirstNew = h.iProcess ? h.iProcess->iCount : 0;
			cpu.Write32(r[1], (uint32_t)n);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecLibraryLookup"))		// (handle, ordinal) -> the export's address (0 = none)
		{
			const int mod = (int)(r[0] & 0xFFFF);
			const TLoaded* m = (r[0] & 0xFFFF0000u) == KHandleLib0 && h.iProcess && mod < h.iProcess->iCount ? &h.iProcess->iMod[mod] : NULL;
			r[0] = m && r[1] >= 1 && r[1] <= m->iExportCount ? cpu.Read32(m->iExportDir + 4 * (r[1] - 1)) : 0;
			return true;
		}
		if (!strcmp(name, "EExecLibraryFileName"))		// (handle, TDes8& name): "<drive>:\sys\bin\<name>", the drive the DLL was found on
		{
			const int mod = (int)(r[0] & 0xFFFF);
			const TLoaded* m = (r[0] & 0xFFFF0000u) == KHandleLib0 && h.iProcess && mod > 0 && mod < h.iProcess->iCount ? &h.iProcess->iMod[mod] : NULL;
			if (!m) { r[0] = (uint32_t)-6; return true; }		// KErrBadHandle
			char s[64];
			snprintf(s, sizeof(s), "%c:\\sys\\bin\\%s", m->iDrive ? m->iDrive : 'Z', m->iName);
			SetDes8(cpu, r[1], s);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecExceptionDescriptor"))	// (code address) -> the exception descriptor of the module holding it (scodeseg.cpp), 0 = none
		{
			const uint32_t a = r[0];
			r[0] = 0;
			for (int m = 0; h.iProcess && m < h.iProcess->iCount; ++m)
				if (a >= h.iProcess->iMod[m].iLoadAddr && a < h.iProcess->iMod[m].iEnd) { r[0] = h.iProcess->iMod[m].iExceptionDescriptor; break; }
			return true;
		}
		if (!strcmp(name, "EExecLibraryType"))		// (handle, TUidType&)
		{
			const int mod = (int)(r[0] & 0xFFFF);
			const TLoaded* m = h.iProcess && mod < h.iProcess->iCount ? &h.iProcess->iMod[mod] : NULL;
			cpu.Write32(r[1], 0x10000079); cpu.Write32(r[1] + 4, 0); cpu.Write32(r[1] + 8, m ? m->iUid3 : 0);
			return true;
		}
		if (!strcmp(name, "EExecHandleClose") && ((r[0] & 0xFFFF0000u) == KHandleLib0 || r[0] == KHandleLoader)) { r[0] = KErrNone; return true; }
		if ((!strcmp(name, "EExecSessionSendSync") || !strcmp(name, "EExecSessionSend")) && r[0] == KHandleLoader)
		{
			int res = KErrNotSupported;
			if ((int)r[1] == -1) res = KErrNone;			// EConnect
			else if (r[1] == 2 && r[2] && h.iProcess)		// ELoadLibrary: (TDes8* TLdrInfo, const TDesC16* file, const TDesC16* path)
			{
				const uint32_t info = cpu.Read32(r[2]);
				char file[160];
				const uint32_t fd = cpu.Read32(r[2] + 4);
				// a TDesC16: GuestDes reads 1 byte per element; read the 16-bit text ourselves
				const uint32_t w = cpu.Read32(fd), type = w >> 28, len = w & 0x0FFFFFFF;
				const uint32_t ptr = type == 0 ? fd + 4 : type == 1 ? cpu.Read32(fd + 4) : type == 2 ? cpu.Read32(fd + 8) : type == 3 ? fd + 8 : cpu.Read32(fd + 8) + 4;
				uint32_t k = 0;
				for (; k < len && k < sizeof(file) - 1; ++k) file[k] = (char)cpu.Read16(ptr + 2 * k);
				file[k] = 0;
				const char* bs = strrchr(file, 0x5C);
				int firstNew = 0;		// (LibraryAttach reports modules from e.iLibFirstNew on)
				const int mod = LoadLibraryModule(cpu, *h.iProcess, bs ? bs + 1 : file, h.iFind, h.iFindCtx, firstNew);
				// the TLdrInfo is in a TPckg (EPtr: [len|type][max][ptr]): iHandle at +16
				const uint32_t li = cpu.Read32(info + 8);
				if (mod >= 0)
				{
					cpu.Write32(li + 16, KHandleLib0 + mod);
					for (int m = 0; m < h.iProcess->iCount; ++m) if (h.iProcess->iMod[m].iEnd > e.iLowWater) e.iLowWater = h.iProcess->iMod[m].iEnd;
				}
				if (h.iLog) { char l[220]; snprintf(l, sizeof(l), "[proteus] RLibrary::Load %s -> %d %s", file, mod, mod < 0 ? h.iProcess->iError : ""); h.iLog(l, h.iLogCtx); }
				res = mod >= 0 ? KErrNone : mod;
			}
			if (r[3]) { cpu.Write32(r[3], (uint32_t)res); if (ipc) ipc->iSignal(ipc->iCtx, 1); }
			r[0] = r[3] ? KErrNone : (uint32_t)res;
			return true;
		}
		if (!strcmp(name, "EExecSessionCreate"))		// (const TDesC8& name, slots, policy, type)
		{
			char srv[160];
			GuestDes(cpu, r[0], srv, sizeof(srv));
			if (!strcmp(srv, "!Loader")) { r[0] = KHandleLoader; return true; }
			if (!ipc) { r[0] = (uint32_t)KErrNotSupported; return true; }
			r[0] = (uint32_t)ipc->iSessionCreate(ipc->iCtx, srv, (int)r[1]);
			return true;
		}
		if (!strcmp(name, "EExecSessionSendSync") || !strcmp(name, "EExecSessionSend"))	// (handle, function, TIpcArgs*, TRequestStatus*)
		{
			if (!ipc || !ipc->iOwns(ipc->iCtx, r[0])) { r[0] = (uint32_t)KErrBadHandle; return true; }
			if (r[3]) cpu.Write32(r[3], 0x80000001u);		// KRequestPending
			r[0] = (uint32_t)ipc->iSend(ipc->iCtx, r[0], (int)r[1], r[2], r[3], !strcmp(name, "EExecSessionSendSync"));
			return true;
		}
		if (!strcmp(name, "EExecSessionShare"))		// (TInt& handle, mode)
		{
			const uint32_t hnd = cpu.Read32(r[0]);
			r[0] = ipc && ipc->iOwns(ipc->iCtx, hnd) ? (uint32_t)ipc->iShare(ipc->iCtx, hnd, (int)r[1]) : (uint32_t)KErrBadHandle;
			return true;
		}
		if (!strcmp(name, "EExecRequestSignal")) { if (ipc) ipc->iSignal(ipc->iCtx, (int)r[0]); return true; }
		if (!strcmp(name, "EExecMutexCreate") || !strcmp(name, "EExecSemaphoreCreate")) { r[0] = KHandleObj + e.iCalls; return true; }
		if (!strcmp(name, "EExecMutexWait") || !strcmp(name, "EExecMutexSignal") || !strcmp(name, "EExecSemaphoreWait") || !strcmp(name, "EExecSemaphoreSignal1")
			|| !strcmp(name, "EExecSemaphoreSignalN")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecProcessCommandLineLength")) { r[0] = 0; return true; }
		if (!strcmp(name, "EExecStaticCallsDone")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecUserThreadExiting")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecLastThreadHandle")) { r[0] = 0; return true; }		// no other handles
		if (!strncmp(name, "EExecBTraceOut", 14) || !strncmp(name, "EExecUTraceOut", 14)) { r[0] = 0; return true; }		// trace off
		if (!strcmp(name, "EExecHalFunction"))		// (group, function, a1, a2): kernel group 0 only -- 7 page size, 8 tick period (us), 10 fast counter Hz
		{
			const uint32_t v = r[0] != 0 ? 0 : r[1] == 7 ? 0x1000 : r[1] == 8 ? 1000 : r[1] == 10 ? 1000000 : 0;
			if (r[2] && v) cpu.Write32(r[2], v);
			r[0] = v ? KErrNone : KErrNotSupported;
			return true;
		}
		if (!strcmp(name, "EExecProcessCommandLine")) { return true; }
		if (!strcmp(name, "EExecThreadId") || !strcmp(name, "EExecProcessId")) { r[0] = 0x100; return true; }
		// ---- the process / thread asking about itself (one process, one thread) ----
		const char* exe = h.iExePath ? h.iExePath : "z:\\sys\\bin\\guest.exe";
		if (!strcmp(name, "EExecProcessFileName") || !strcmp(name, "EExecHandleFullName") || !strcmp(name, "EExecHandleName"))	// (handle, TDes8&)
		{
			const char* s = exe;
			if (strcmp(name, "EExecProcessFileName"))			// a name: "about[10005a22]0001" style is not needed -- the file's base name
			{
				const char* b = strrchr(exe, '\\');
				s = b ? b + 1 : exe;
			}
			SetDes8(cpu, r[1], s);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecProcessType"))		// (handle, TUidType&)
		{
			const TLoaded* m = h.iImage;
			cpu.Write32(r[1], m ? m->iUid1 : 0x1000007a); cpu.Write32(r[1] + 4, 0x100039ce); cpu.Write32(r[1] + 8, m ? m->iUid3 : 0);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecProcessSecurityInfo") || !strcmp(name, "EExecThreadSecurityInfo") || !strcmp(name, "EExecCreatorSecurityInfo"))
		{
			// SSecurityInfo: iSecureId, iVendorId, iCaps (2 words): the guest gets proteus.exe's capabilities (All -TCB)
			const uint32_t si = strcmp(name, "EExecCreatorSecurityInfo") ? r[1] : r[0];
			cpu.Write32(si, h.iImage ? h.iImage->iUid3 : 0); cpu.Write32(si + 4, 0); cpu.Write32(si + 8, 0xFFFFFFFEu); cpu.Write32(si + 12, 0x7FFFFFFFu);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecProcessPriority")) { r[0] = 350; return true; }		// EPriorityForeground
		if (!strcmp(name, "EExecThreadPriority")) { r[0] = 0; return true; }			// EPriorityNormal
		if (!strcmp(name, "EExecThreadProcessPriority")) { r[0] = 350; return true; }
		if (!strcmp(name, "EExecProcessSetPriority") || !strcmp(name, "EExecThreadSetPriority") || !strcmp(name, "EExecThreadSetProcessPriority")
			|| !strcmp(name, "EExecProcessSetFlags") || !strcmp(name, "EExecThreadSetFlags") || !strcmp(name, "EExecFsRegisterThread")
			|| !strcmp(name, "EExecWsRegisterThread") || !strcmp(name, "EExecThreadRendezvous") || !strcmp(name, "EExecProcessRendezvous")
			|| !strcmp(name, "EExecThreadRename") || !strcmp(name, "EExecProcessRename")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecProcessFlags") || !strcmp(name, "EExecThreadFlags") || !strcmp(name, "EExecThreadRequestCount")) { r[0] = 0; return true; }
		if (!strcmp(name, "EExecThreadGetCpuTime")) { cpu.Write32(r[1], 0); cpu.Write32(r[1] + 4, 0); r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecThreadStackInfo"))		// (handle, TThreadStackInfo&): iBase (top), iLimit, iExpandLimit
		{
			const uint32_t top = (cpu.iBase + cpu.iSize) & ~0xFFFu;
			cpu.Write32(r[1], top); cpu.Write32(r[1] + 4, top - KStackBytes); cpu.Write32(r[1] + 8, top - KStackBytes);
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecProcessGetDataParameter") || !strcmp(name, "EExecProcessGetHandleParameter")
			|| !strcmp(name, "EExecProcessDataParameterLength")) { r[0] = (uint32_t)KErrNotFound; return true; }
		if (!strcmp(name, "EExecThreadRequestSignal")) { if (ipc) ipc->iSignal(ipc->iCtx, 1); return true; }
		if (!strcmp(name, "EExecThreadProcess") || !strcmp(name, "EExecThreadAsProcess")) { r[0] = 0xFFFF8000u; return true; }	// KCurrentProcessHandle
		if (!strcmp(name, "EExecProcessGetMemoryInfo")) { r[0] = (uint32_t)KErrNotSupported; return true; }
		if (!strcmp(name, "EExecProcessExitType")) { r[0] = 2; return true; }		// EExitPending
		if (!strcmp(name, "EExecThreadKill") || !strcmp(name, "EExecProcessKill"))
		{
			// (handle, TExitType, reason, const TDesC8* category): Kill / Terminate / Panic of the current thread or process
			if (r[1] != 0 && h.iLog)
			{
				char cat[48] = "";
				if (r[3]) GuestDes(cpu, r[3], cat, sizeof(cat));
				char l[120];
				snprintf(l, sizeof(l), "[proteus] guest %s: %s %d", r[1] == 2 ? "PANIC" : "terminate", cat, (int)r[2]);
				h.iLog(l, h.iLogCtx);
				Backtrace(cpu, h);
			}
			cpu.Halt(EExited, (int32_t)r[2]);
			return true;
		}
		if (!strcmp(name, "EExecLibraryCallList") || !strcmp(name, "EExecStaticCallList")) { cpu.Write32(r[0], 0); r[0] = KErrNone; return true; }
	}
	char note[96];
	snprintf(note, sizeof(note), "%s exec %u (%s) is not emulated", fast ? "fast" : "slow", n, name);
	if (h.iLog) { char l[120]; snprintf(l, sizeof(l), "[proteus] %s", note); h.iLog(l, h.iLogCtx); }
	strncpy(cpu.iNote, note, sizeof(cpu.iNote) - 1);
	cpu.Halt(EHleStop);
	return true;
}
}

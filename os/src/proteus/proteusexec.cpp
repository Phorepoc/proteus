// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusexec.cpp -- OURS (Proteus P2): the executive (SWI) side of a guest that runs the REAL ARM euser.dll (our 2010-source device build,
// emitter/euser_dev.dll). euser's Exec:: stubs are `swi n` (slow) / `swi 0x800000|n` (fast), arguments r0-r3, result r0
// (baseline u32exec.h, numbers in toolchain/generated/exec_enum.h -> exec_names.inc). This is a user-mode stand-in for the kernel of ONE
// single-threaded process: thread-local executive state (heap, trap handler, active scheduler, TLS), chunks carved from the top of the guest
// region, handles as small integers, RDebug output to the log. Unknown calls stop the guest with their name (the next thing to emulate).
#include "proteusrt.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace proteus
{
#include "exec_names.inc"

enum { KFast = 0x800000 };
enum { KErrNone = 0, KErrNotFound = -1, KErrNotSupported = -5, KErrNoMemory = -4, KErrArgument = -6, KErrBadHandle = -8, KErrCancel = -3, KErrOverflow = -9, KErrUnderflow = -10 };

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

// memory Proteus keeps for itself in the guest (strings the native GL returns): one 1 MB chunk, its pages committed as it fills
uint32_t GuestBytes(CArmCpu& cpu, TExecState& e, const void* aSrc, uint32_t aLen)
{
	static int chunk = -1;
	static uint32_t used;
	const uint32_t KBytes = 1 << 20;
	if (chunk < 0)
	{
		const int h = NewChunk(cpu, e, KBytes);
		if (h < 0) return 0;
		chunk = h - KHandleChunk0;
	}
	const uint32_t at = (used + 3) & ~3u;
	if (aLen > KBytes - at) return 0;
	for (uint32_t p = at >> 12; p <= (at + aLen - 1) >> 12; ++p)
		if (!e.iChunkPages[chunk][p] && CommitPages(cpu, e, chunk, p << 12, 0x1000) != KErrNone) return 0;
	const uint32_t g = e.iChunkBase[chunk] + at;
	for (uint32_t k = 0; k < aLen; ++k) cpu.Write8(g + k, ((const uint8_t*)aSrc)[k]);
	used = at + aLen;
	return g;
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

// ---- guest threads (proteusrt.h: TGuestThread) ----
enum { KErrGeneral = -2, KErrTimedOut = -33, KCurrentThreadHandle = (int)0xFFFF8001, KRequestPending = (int)0x80000001 };
static const uint32_t KLocalThreadData = 48;		// as proteusrt.cpp's StartThreadStack: TLocalThreadData at the top of the stack
static const uint32_t KMaxThreadCreateInfo = 256;	// u32std.h

// the host clock for the wake-up latency statistics (set from every scheduler entry that has the THle)
static uint64_t (*gClockFn)(void*);
static void* gClockCtx;
static uint64_t ClockUs() { return gClockFn ? gClockFn(gClockCtx) : 0; }

static void SaveThread(CArmCpu& c, TExecState& e, TGuestThread& t)
{
	memcpy(t.iR, c.R, sizeof(t.iR));
	t.iN = c.iN; t.iZ = c.iZ; t.iC = c.iC; t.iV = c.iV; t.iT = c.iT; t.iGE = c.iGE;
	t.iTpidrurw = c.iTpidrurw; t.iTpidruro = c.iTpidruro; t.iFpscr = c.iFpscr; t.iFpexc = c.iFpexc;
	memcpy(t.iD, c.iD, sizeof(t.iD));
	t.iHeap = e.iHeap; t.iTrapHandler = e.iTrapHandler; t.iActiveScheduler = e.iActiveScheduler; t.iTrapFrame = e.iTrapFrame;
	memcpy(t.iTlsKey, e.iTlsKey, sizeof(t.iTlsKey)); memcpy(t.iTlsVal, e.iTlsVal, sizeof(t.iTlsVal)); t.iTlsCount = e.iTlsCount;
}

static void LoadThread(CArmCpu& c, TExecState& e, const TGuestThread& t)
{
	memcpy(c.R, t.iR, sizeof(c.R));
	c.iN = t.iN; c.iZ = t.iZ; c.iC = t.iC; c.iV = t.iV; c.iT = t.iT; c.iGE = t.iGE;
	c.iTpidrurw = t.iTpidrurw; c.iTpidruro = t.iTpidruro; c.iFpscr = t.iFpscr; c.iFpexc = t.iFpexc;
	memcpy(c.iD, t.iD, sizeof(c.iD));
	c.iExcl = false;		// a thread switch clears the exclusive monitor
	e.iSliceStart = c.iSteps;
	if (t.iReadyAt)			// the wake-up latency: from ready to running
	{
		TGuestThread& w = const_cast<TGuestThread&>(t);
		const uint64_t now = ClockUs(), lat = now > w.iReadyAt ? now - w.iReadyAt : 0;
		w.iReadyAt = 0; ++w.iWakeN; w.iWakeSum += lat;
		if (lat > w.iWakeMax) w.iWakeMax = lat;
		if (lat > 10000) ++w.iWakeSlow;
	}
	e.iHeap = t.iHeap; e.iTrapHandler = t.iTrapHandler; e.iActiveScheduler = t.iActiveScheduler; e.iTrapFrame = t.iTrapFrame;
	memcpy(e.iTlsKey, t.iTlsKey, sizeof(e.iTlsKey)); memcpy(e.iTlsVal, t.iTlsVal, sizeof(e.iTlsVal)); e.iTlsCount = t.iTlsCount;
}

// a thread handle (KCurrentThreadHandle or KHandleThread0 + i) -> thread index, -1 = not a guest thread
static int ThreadIndex(TExecState& e, uint32_t aHandle)
{
	if (aHandle == (uint32_t)KCurrentThreadHandle) return e.iCur;
	const uint32_t i = aHandle - KHandleThread0;
	return e.iThread && i < (uint32_t)e.iThreadCount && e.iThread[i].iState != EThreadFree ? (int)i : -1;
}

// a free slot of the sync object table (a closed object's), else a new one: the table grows with the guest (the kernel has no count limit
// either; Qt creates a condition variable or mutex per object and closes them), up to what a handle can name. -1 = full.
static int NewSync(TExecState& e)
{
	for (int i = 0; i < e.iSyncCount; ++i)
		if (!e.iSync[i].iType) { memset(&e.iSync[i], 0, sizeof(TSyncObj)); e.iSync[i].iRefs = 1; return i; }
	if (e.iSyncCount >= KMaxSync) return -1;
	if (e.iSyncCount >= e.iSyncCap)
	{
		const int cap = e.iSyncCap * 2 > KMaxSync ? KMaxSync : e.iSyncCap * 2;
		TSyncObj* n = (TSyncObj*)realloc(e.iSync, (size_t)cap * sizeof(TSyncObj));
		if (!n) return -1;
		memset(n + e.iSyncCap, 0, (size_t)(cap - e.iSyncCap) * sizeof(TSyncObj));
		e.iSync = n; e.iSyncCap = cap;
	}
	memset(&e.iSync[e.iSyncCount], 0, sizeof(TSyncObj));
	e.iSync[e.iSyncCount].iRefs = 1;
	return e.iSyncCount++;
}

static int SyncIndex(TExecState& e, uint32_t aHandle, int aType)
{
	const uint32_t i = aHandle - KHandleObj;
	return e.iSync && i < (uint32_t)e.iSyncCount && e.iSync[i].iType == aType ? (int)i : -1;
}

// a blocked thread becomes ready; its wait returns aResult (never the running thread: that one is not blocked)
static void Wake(TExecState& e, int i, int32_t aResult)
{
	TGuestThread& t = e.iThread[i];
	t.iR[0] = (uint32_t)aResult;
	t.iState = EThreadReady;
	t.iDeadline = 0;
	t.iReadyAt = ClockUs();
}

// DThread::RequestSignal: the thread's request semaphore; a thread blocked in WaitForAnyRequest takes the signal and runs
static void SignalThread(TExecState& e, int i, int aCount)
{
	if (i < 0 || i >= e.iThreadCount || e.iThread[i].iState == EThreadDead || aCount <= 0) return;
	TGuestThread& t = e.iThread[i];
	t.iRequests += aCount;
	if (t.iState == EThreadWaitAny) { --t.iRequests; t.iState = EThreadReady; t.iReadyAt = ClockUs(); }
}

static void CompleteFor(CArmCpu& cpu, TExecState& e, int aThread, uint32_t aStatus, int32_t aValue)
{
	if (!aStatus) return;
	cpu.Write32(aStatus, (uint32_t)aValue);
	// a guest-completed status is no outstanding native request: left noted, a later native signal would be credited to it (AttributeNative
	// takes the first noted status that is no longer pending) and its owner would see a stray signal
	for (int k = 0; k < KMaxReqs; ++k)
		if (e.iReqStatus[k] == aStatus && !e.iReqDone[k]) e.iReqStatus[k] = 0;
	SignalThread(e, aThread, 1);
}

static bool TakeMutex(TExecState& e, int m, int i)
{
	TSyncObj& s = e.iSync[m];
	if (s.iHolder >= 0 && s.iHolder != i) return false;
	s.iHolder = i;
	++s.iCount;
	return true;
}

// the oldest thread blocked in state aState on object aObj, -1 = none
static int OldestWaiter(TExecState& e, int aState, int aObj)
{
	int best = -1;
	for (int i = 0; i < e.iThreadCount; ++i)
		if (e.iThread[i].iState == aState && e.iThread[i].iWaitObj == aObj && (best < 0 || e.iThread[i].iWaitSeq < e.iThread[best].iWaitSeq)) best = i;
	return best;
}

static void Block(TExecState& e, int i, int aState, int aObj, uint64_t aDeadline)
{
	TGuestThread& t = e.iThread[i];
	t.iState = aState;
	t.iWaitObj = aObj;
	t.iWaitSeq = ++e.iWaitSeq;
	t.iDeadline = aDeadline;
}

// a free mutex goes to its oldest waiter (a plain MutexWait, or a condvar waiter taking its mutex back with its saved hold count)
static void HandOffMutex(TExecState& e, int m)
{
	TSyncObj& s = e.iSync[m];
	if (s.iHolder >= 0) return;
	const int w = OldestWaiter(e, EThreadWaitMutex, m);
	if (w < 0) return;
	TGuestThread& t = e.iThread[w];
	s.iHolder = w;
	s.iCount = t.iCondCount > 0 ? t.iCondCount : 1;
	const int32_t res = t.iWaitResult;
	t.iCondCount = 0;
	t.iWaitResult = KErrNone;
	Wake(e, w, res);
}

// a condvar waiter was signalled or timed out: it takes its mutex back before its Wait returns (DCondVar)
static void CondToMutex(TExecState& e, int i, int32_t aResult)
{
	TGuestThread& t = e.iThread[i];
	const int m = t.iCondMutex;
	t.iWaitResult = aResult;
	if (TakeMutex(e, m, i))
	{
		e.iSync[m].iCount = t.iCondCount > 0 ? t.iCondCount : 1;
		t.iCondCount = 0;
		t.iWaitResult = KErrNone;
		Wake(e, i, aResult);
	}
	else
		Block(e, i, EThreadWaitMutex, m, 0);
}

// a timed wait ran out
static void Expire(TExecState& e, int i)
{
	if (e.iThread[i].iState == EThreadWaitCond) CondToMutex(e, i, KErrTimedOut);
	else Wake(e, i, KErrTimedOut);
}

static uint64_t NowUs(THle& h) { return h.iIpc && h.iIpc->iNowUs ? h.iIpc->iNowUs(h.iIpc->iCtx) : 0; }

// the thread's own timer ran out (DThread::iTimer, CompleteUserAfter): complete its status KErrNone and signal it, like any request
static bool FireAfter(CArmCpu& cpu, TExecState& e, int i, uint64_t aNow)
{
	TGuestThread& t = e.iThread[i];
	if (!t.iAfterStatus || t.iState == EThreadDead || (aNow && t.iAfterAt > aNow)) return false;
	cpu.Write32(t.iAfterStatus, 0);
	t.iAfterStatus = 0;
	SignalThread(e, i, 1);
	return true;
}

// a wait timeout in microseconds (0 = forever, as ExecHandler::MutexWait) -> a deadline on the iNowUs clock (0 = none)
static uint64_t Deadline(THle& h, int32_t aTimeout) { return aTimeout > 0 ? NowUs(h) + (uint64_t)aTimeout : 0; }

// a request a guest thread handed to the native side: its completion signal belongs to that thread
static void NoteRequest(THle& h, const char* aName, uint32_t aStatus)
{
	TExecState& e = h.iExec;
	if (!aStatus || !e.iThread) return;
	static int noteLines = 0;
	if (h.iLog && noteLines < 4000)		// trace: a request handed to the native side
	{
		++noteLines;
		char l[120];
		snprintf(l, sizeof(l), "[proteus] sig note: %s status %x thread %d", aName, aStatus, e.iCur);
		h.iLog(l, h.iLogCtx);
	}
	int free = -1;
	// A guest starts a request on a status only when its earlier request has completed (a cancel completes it too): an entry still open for the same
	// status completed, and the guest saw it without taking its signal (Qt's RunIfReady runs a completed active object that way). Its signal is still
	// to come; the status now belongs to the new request, so the old entry is marked owed and the new one is noted beside it.
	for (int k = 0; k < KMaxReqs; ++k)
		if (e.iReqStatus[k] == aStatus && !e.iReqDone[k]) e.iReqDone[k] = true;
	for (int k = 0; k < KMaxReqs; ++k)
		if (!e.iReqStatus[k] && free < 0) free = k;
	if (free < 0)		// full: forget the oldest
	{
		memmove(e.iReqStatus, e.iReqStatus + 1, sizeof(e.iReqStatus) - sizeof(e.iReqStatus[0]));
		memmove(e.iReqThread, e.iReqThread + 1, sizeof(e.iReqThread) - sizeof(e.iReqThread[0]));
		memmove(e.iReqDone, e.iReqDone + 1, sizeof(e.iReqDone) - sizeof(e.iReqDone[0]));
		free = KMaxReqs - 1;
	}
	e.iReqStatus[free] = aStatus;
	e.iReqThread[free] = e.iCur;
	e.iReqDone[free] = false;
}

// one native request signal arrived: credit the thread whose noted request completed; else the only thread waiting for one; else the main thread
static void AttributeNative(CArmCpu& cpu, THle& h)
{
	TExecState& e = h.iExec;
	int who = -1;
	for (int k = 0; k < KMaxReqs && who < 0; ++k)
		if (e.iReqStatus[k] && e.iReqDone[k] && e.iThread[e.iReqThread[k]].iState != EThreadDead)
		{
			who = e.iReqThread[k];
			static int owedLines = 0;
			if (h.iLog && owedLines < 1000)		// trace: a signal owed by a request whose status was reused before it was credited
			{
				++owedLines;
				char l[96];
				snprintf(l, sizeof(l), "[proteus] sig credit: owed, status %x of thread %d", e.iReqStatus[k], who);
				h.iLog(l, h.iLogCtx);
			}
			e.iReqStatus[k] = 0;
			e.iReqDone[k] = false;
		}
	for (int k = 0; k < KMaxReqs && who < 0; ++k)
	{
		const uint32_t st = e.iReqStatus[k];
		if (!st) continue;
		if (e.iReqDone[k]) { e.iReqStatus[k] = 0; e.iReqDone[k] = false; continue; }
		if (e.iThread[e.iReqThread[k]].iState == EThreadDead || !cpu.Valid(st, 4)) { e.iReqStatus[k] = 0; continue; }
		const int32_t v = (int32_t)cpu.Read32(st);
		if (v == KRequestPending) continue;
		who = e.iReqThread[k];
		e.iReqStatus[k] = 0;
		e.iReqDone[k] = false;
		static int creditLines = 0;
		if (h.iLog && creditLines < 4000)		// trace: which noted request explains this signal
		{
			++creditLines;
			char l[96];
			snprintf(l, sizeof(l), "[proteus] sig credit: status %x of thread %d = %x", st, who, (unsigned)v);
			h.iLog(l, h.iLogCtx);
		}
		static int errLines = 0;
		if (v < 0 && h.iLog && errLines < 100)		// trace: a native request of the guest that completed with an error
		{
			++errLines;
			char l[96];
			snprintf(l, sizeof(l), "[proteus] request %x of thread %d completed %d", st, who, (int)v);
			h.iLog(l, h.iLogCtx);
		}
	}
	if (who < 0)
	{
		int waiting = -1, n = 0;
		for (int i = 0; i < e.iThreadCount; ++i) if (e.iThread[i].iState == EThreadWaitAny) { if (waiting < 0) waiting = i; ++n; }
		who = n == 1 ? waiting : (e.iThread[0].iState == EThreadWaitAny || waiting < 0) ? 0 : waiting;
		static int guesses = 0;
		if (h.iLog && guesses < 30)		// trace: a native signal no noted request explains (credited by guess)
		{
			++guesses;
			char l[96];
			snprintf(l, sizeof(l), "[proteus] native signal with no completed noted request: credited to thread %d (cur %d)", who, e.iCur);
			h.iLog(l, h.iLogCtx);
			for (int k = 0; k < KMaxReqs; ++k)		// the requests still noted, and what they hold now
			{
				if (!e.iReqStatus[k]) continue;
				snprintf(l, sizeof(l), "[proteus]   noted %x thread %d value %x", e.iReqStatus[k], e.iReqThread[k],
					cpu.Valid(e.iReqStatus[k], 4) ? cpu.Read32(e.iReqStatus[k]) : 0xDEAD);
				h.iLog(l, h.iLogCtx);
			}
		}
	}
	SignalThread(e, who, 1);
}

// the running thread no longer runs (blocked / dead): run the next ready one, round robin; with none ready, expire timed waits or wait for
// the native kernel (a server / timer completing a request). All blocked with nothing to wait for = a deadlock: the guest stops.
static void Reschedule(CArmCpu& cpu, THle& h)
{
	TExecState& e = h.iExec;
	if (h.iIpc) { gClockFn = h.iIpc->iNowUs; gClockCtx = h.iIpc->iCtx; }
	e.iThread[e.iCur].iRan += cpu.iSteps - e.iSliceStart;
	SaveThread(cpu, e, e.iThread[e.iCur]);
	for (;;)
	{
		for (int k = 1; k <= e.iThreadCount; ++k)
		{
			const int i = (e.iCur + k) % e.iThreadCount;
			if (e.iThread[i].iState != EThreadReady) continue;
			e.iCur = i;
			LoadThread(cpu, e, e.iThread[i]);
			return;
		}
		const uint64_t now = NowUs(h);
		uint64_t next = 0;
		int nextThread = -1;
		bool timed = false;
		bool expired = false, waitAny = false;
		for (int i = 0; i < e.iThreadCount; ++i)
		{
			TGuestThread& t = e.iThread[i];
			if (t.iState == EThreadWaitAny) waitAny = true;
			if (t.iAfterStatus && t.iState != EThreadDead)
			{
				if (!now || t.iAfterAt <= now) { if (FireAfter(cpu, e, i, now ? now : ~0ull)) expired = true; }
				else if (!next || t.iAfterAt < next) { next = t.iAfterAt; timed = true; }
			}
			if (!t.iDeadline || (t.iState != EThreadWaitMutex && t.iState != EThreadWaitSem && t.iState != EThreadWaitCond)) continue;
			if (now && t.iDeadline <= now) { Expire(e, i); expired = true; }
			else if (!next || t.iDeadline < next) { next = t.iDeadline; nextThread = i; timed = true; }
		}
		if (expired) continue;
		if (!h.iIpc || !h.iIpc->iNowUs)		// no native kernel to wait for: time passes only when nothing can run
		{
			if (nextThread >= 0) { Expire(e, nextThread); continue; }
			strncpy(cpu.iNote, waitAny ? "WaitForAnyRequest with nothing to wait for" : "all guest threads are blocked (deadlock)", sizeof(cpu.iNote) - 1);
			cpu.Halt(EHleStop);
			return;
		}
		if (!waitAny && !timed)
		{
			strncpy(cpu.iNote, "all guest threads are blocked (deadlock)", sizeof(cpu.iNote) - 1);
			cpu.Halt(EHleStop);
			return;
		}
		bool got = true;
		const uint64_t waitFrom = NowUs(h);
		if (timed)
		{
			const uint64_t d = next - now;
			got = h.iIpc->iWaitAnyFor(h.iIpc->iCtx, d > 0x7FFFFFFF ? 0x7FFFFFFF : (int)d);
		}
		else
			h.iIpc->iWaitAny(h.iIpc->iCtx);
		e.iWaitUs += NowUs(h) - waitFrom;
		if (got) AttributeNative(cpu, h);
	}
}

// the running thread blocks: the scheduler runs another (or this one again once it is woken)
static void BlockCurrent(CArmCpu& cpu, THle& h, int aState, int aObj, uint64_t aDeadline)
{
	Block(h.iExec, h.iExec.iCur, aState, aObj, aDeadline);
	Reschedule(cpu, h);
}

// The end of a time slice. The kernel is preemptive: a thread that never blocks (a loader decoding data) must not starve the others, and a
// native server's reply must reach its thread while another one runs -- the main thread's active scheduler answers the view server in time.
// So: native request completions that have arrived are credited (never waiting for one), timed waits that ran out expire, and when
// another thread is ready the running one goes to the back of the line (the running thread stays EThreadReady).
static void HostedSweep(CArmCpu& cpu, THle& h);

void Preempt(CArmCpu& cpu, THle& h)
{
	TExecState& e = h.iExec;
	if (h.iIpc) { gClockFn = h.iIpc->iNowUs; gClockCtx = h.iIpc->iCtx; }
	if (!e.iThread || e.iThreadCount < 1) return;
	if (h.iIpc && h.iIpc->iPoll)
		for (int n = 0; n < KMaxReqs && h.iIpc->iPoll(h.iIpc->iCtx); ++n) AttributeNative(cpu, h);
	HostedSweep(cpu, h);
	uint64_t now = 0;
	for (int i = 0; i < e.iThreadCount; ++i)
	{
		TGuestThread& t = e.iThread[i];
		if (!t.iDeadline || (t.iState != EThreadWaitMutex && t.iState != EThreadWaitSem && t.iState != EThreadWaitCond)) continue;
		if (!now) now = NowUs(h);
		if (now && t.iDeadline <= now) Expire(e, i);
	}
	for (int i = 0; i < e.iThreadCount; ++i)
		if (e.iThread[i].iAfterStatus)
		{
			if (!now) now = NowUs(h);
			if (now) FireAfter(cpu, e, i, now);
		}
	for (int k = 1; k < e.iThreadCount; ++k)
	{
		const int i = (e.iCur + k) % e.iThreadCount;
		if (e.iThread[i].iState != EThreadReady) continue;
		e.iThread[e.iCur].iRan += cpu.iSteps - e.iSliceStart;
		SaveThread(cpu, e, e.iThread[e.iCur]);
		e.iCur = i;
		LoadThread(cpu, e, e.iThread[i]);
		return;
	}
}

void ThreadStats(CArmCpu& cpu, THle& h, char* aOut, int aMax)
{
	TExecState& e = h.iExec;
	int n = snprintf(aOut, aMax, "cur=%d wait=%ums slow=%uK svc=%uK trap=%uK mem=%uK ran(M):", e.iCur, (unsigned)(e.iWaitUs / 1000), (unsigned)(cpu.iStatSlow >> 10), (unsigned)(cpu.iStatSvc >> 10), (unsigned)(cpu.iStatTrap >> 10), (unsigned)(cpu.iStatMem >> 10));
	for (int i = 0; i < e.iThreadCount && n > 0 && n < aMax - 16; ++i)
	{
		const uint64_t ran = e.iThread[i].iRan + (i == e.iCur ? cpu.iSteps - e.iSliceStart : 0);
		n += snprintf(aOut + n, aMax - n, " %d:%u%s", i, (unsigned)(ran >> 20), e.iThread[i].iState == EThreadDead ? "x" : "");
	}
	for (int i = 0; i < e.iThreadCount && n > 0 && n < aMax - 48; ++i)		// wake-up latency (ready -> running) per thread: n/avg us/max us/over 10 ms
		if (e.iThread[i].iWakeN)
			n += snprintf(aOut + n, aMax - n, " wake%d=%u/%u/%u/%u", i, e.iThread[i].iWakeN, (unsigned)(e.iThread[i].iWakeSum / e.iThread[i].iWakeN),
				(unsigned)e.iThread[i].iWakeMax, e.iThread[i].iWakeSlow);
}

static void GThreadDied(CArmCpu& cpu, TExecState& e, int aThread);		// the guest servers a thread owns (below)

// a thread ends (ExecHandler::ThreadKill): logons complete with the exit reason, mutexes it held go to their waiters
static void EndThread(CArmCpu& cpu, THle& h, int i, int aType, int aReason, const char* aCategory)
{
	TExecState& e = h.iExec;
	TGuestThread& t = e.iThread[i];
	t.iState = EThreadDead;
	t.iExitType = aType;
	t.iExitReason = aReason;
	strncpy(t.iExitCategory, aCategory, sizeof(t.iExitCategory) - 1);
	t.iStackFree = t.iStackChunk >= 0;
	for (int k = 0; k < KMaxLogons; ++k)
		if (t.iLogonStatus[k]) { CompleteFor(cpu, e, t.iLogonOwner[k], t.iLogonStatus[k], aReason); t.iLogonStatus[k] = 0; }
	for (int m = 0; m < e.iSyncCount; ++m)
		if (e.iSync[m].iType == 1 && e.iSync[m].iHolder == i) { e.iSync[m].iHolder = -1; e.iSync[m].iCount = 0; HandOffMutex(e, m); }
	GThreadDied(cpu, e, i);
}

// ExecHandler::ThreadCreate: a new thread with its own stack chunk, suspended; it starts as the kernel starts a user thread (ckernel.cia):
// at the process re-entry point with r4 = KModuleEntryReasonThreadInit and sp -> a copy of the creation info (_E32Startup hands it to RunThread)
static int CreateThread(CArmCpu& cpu, THle& h, uint32_t aInfo)
{
	TExecState& e = h.iExec;
	// the stack chunk: the requested size plus 4 KB above the thread data, as the main thread's (StartThreadStack: euser's leave trace scans
	// stack words upward from sp)
	const uint32_t total = cpu.Read32(aInfo + 44), stack = ((cpu.Read32(aInfo + 28) + 0xFFF) & ~0xFFFu) + 0x1000;
	if (total < 48 || total > KMaxThreadCreateInfo || (total & 7) || stack < 2 * KMaxThreadCreateInfo) return KErrArgument;
	// the slot of a dead natively hosted thread (an MMF controller thread: one per played file, the guest closed its handle when its proxy closed)
	// is reused with its stack; other dead threads keep theirs (a stale handle to them still answers ExitType)
	int reuse = -1;
	for (int i = 1; i < e.iThreadCount && reuse < 0; ++i)
		if (e.iThread[i].iHosted && e.iThread[i].iState == EThreadDead && e.iThread[i].iStackFree && e.iChunkMax[e.iThread[i].iStackChunk] >= stack) reuse = i;
	if (reuse < 0 && e.iThreadCount >= KMaxThreads) return KErrNoMemory;
	int chunk = -1;
	if (reuse >= 0) { chunk = e.iThread[reuse].iStackChunk; e.iThread[reuse].iStackFree = false; }
	for (int i = 0; i < e.iThreadCount && chunk < 0; ++i)		// a dead thread's stack, if it is big enough
		if (e.iThread[i].iStackFree && e.iChunkMax[e.iThread[i].iStackChunk] >= stack) { chunk = e.iThread[i].iStackChunk; e.iThread[i].iStackFree = false; }
	if (chunk < 0)
	{
		const int hnd = NewChunk(cpu, e, stack);
		if (hnd < 0) return hnd;
		chunk = ChunkIndex(e, (uint32_t)hnd);
		const int r = CommitPages(cpu, e, chunk, 0, e.iChunkMax[chunk]);
		if (r < 0) return r;
	}
	const int idx = reuse >= 0 ? reuse : e.iThreadCount++;
	TGuestThread& t = e.iThread[idx];
	memset(&t, 0, sizeof(t));
	t.iId = 0x7F000000u + idx;
	t.iStackChunk = chunk;
	const uint32_t base = e.iChunkBase[chunk], size = e.iChunkMax[chunk], tld = base + size - 0x1000 - KLocalThreadData;
	for (uint32_t a = tld; a < tld + KLocalThreadData; a += 4) cpu.Write32(a, 0);
	cpu.Write32(tld + 12, t.iId);							// TLocalThreadData::iThreadId
	const uint32_t sp = (tld - total) & ~7u;
	for (uint32_t o = 0; o < total; o += 4) cpu.Write32(sp + o, cpu.Read32(aInfo + o));
	cpu.Write32(sp + 24, base);								// iUserStack
	cpu.Write32(sp + 28, size - 0x1000);					// iUserStackSize
	const uint32_t entry = e.iReentry ? e.iReentry : h.iProcess->iMod[0].iEntry;
	t.iR[4] = 1;											// KModuleEntryReasonThreadInit
	t.iR[13] = sp;
	t.iR[15] = entry & ~1u;
	t.iT = entry & 1;
	t.iTpidrurw = tld;
	t.iState = EThreadSuspended;
	{
		char fn[120], nm[40];
		Where(*h.iProcess, cpu, cpu.Read32(aInfo + 8), fn, sizeof(fn));
		const uint32_t nl = cpu.Read32(aInfo + 36) & 0xFFFFFF, np = cpu.Read32(aInfo + 40);
		int k = 0;
		for (; k < (int)sizeof(nm) - 1 && (uint32_t)k < nl && cpu.Valid(np + k, 1); ++k) { const uint8_t c = cpu.Read8(np + k); nm[k] = c >= 32 && c < 127 ? (char)c : '?'; }
		nm[k] = 0;
		char l[240];
		snprintf(l, sizeof(l), "[proteus] thread %d '%s' created by %d, function %s", idx, nm, e.iCur, fn);
		if (h.iLog) h.iLog(l, h.iLogCtx);
	}
	// a thread the system runs natively (the MMF controller thread, in the module mmfcontrollerframework): the bridge claims it by the module
	// its entry function is in; it is then never scheduled -- its Resume starts the native body (HostedResume)
	if (h.iIpc && h.iIpc->iHostThread)
	{
		const uint32_t fn = cpu.Read32(aInfo + 8) & ~1u;
		for (int m = 0; m < h.iProcess->iCount; ++m)
			if (fn >= h.iProcess->iMod[m].iLoadAddr && fn < h.iProcess->iMod[m].iEnd)
			{
				if (h.iIpc->iHostThread(h.iIpc->iCtx, h.iProcess->iMod[m].iName, idx, cpu.Read32(aInfo + 12)))
				{
					t.iHosted = true;
					if (h.iLog) { char l[120]; snprintf(l, sizeof(l), "[proteus] thread %d is hosted natively (entry in %s)", idx, h.iProcess->iMod[m].iName); h.iLog(l, h.iLogCtx); }
				}
				break;
			}
	}
	return (int)(KHandleThread0 + idx);
}

// Resume of a natively hosted thread: the first one starts it, and the rendezvous logons complete with the result (as the thread's own
// RThread::Rendezvous would). Later ones (RMMFControllerProxy::SetThreadPriority: Suspend / SetPriority / Resume) change nothing.
static void HostedResume(CArmCpu& cpu, THle& h, int i)
{
	TExecState& e = h.iExec;
	TGuestThread& t = e.iThread[i];
	if (t.iHostStarted || t.iState == EThreadDead) return;
	t.iHostStarted = true;
	const int r = h.iIpc->iHostStart(h.iIpc->iCtx, i);
	for (int k = 0; k < KMaxLogons; ++k)
		if (t.iLogonStatus[k] && t.iLogonRendezvous[k]) { CompleteFor(cpu, e, t.iLogonOwner[k], t.iLogonStatus[k], r); t.iLogonStatus[k] = 0; }
	if (r != KErrNone) EndThread(cpu, h, i, 0, r, "");
}

// the hosted threads that have ended natively (the controller closed with its session, or died): the guest sees them exit
static void HostedSweep(CArmCpu& cpu, THle& h)
{
	TExecState& e = h.iExec;
	if (!h.iIpc || !h.iIpc->iHostExit) return;
	for (int i = 0; i < e.iThreadCount; ++i)
	{
		TGuestThread& t = e.iThread[i];
		int type = 0, reason = 0;
		if (t.iHosted && t.iHostStarted && t.iState != EThreadDead && h.iIpc->iHostExit(h.iIpc->iCtx, i, &type, &reason))
		{
			if (h.iLog) { char l[100]; snprintf(l, sizeof(l), "[proteus] hosted thread %d ended: type %d reason %d", i, type, reason); h.iLog(l, h.iLogCtx); }
			EndThread(cpu, h, i, type, reason, "");
		}
	}
}

// the thread / synchronisation / request-semaphore executive calls; false = not one of them
int gExecTraceWindow;
static bool ThreadExec(CArmCpu& cpu, THle& h, const char* name, uint32_t* r)
{
	TExecState& e = h.iExec;
	if (!e.iThread) return false;
	const int cur = e.iCur;
	if (!strcmp(name, "EFastExecWaitForAnyRequest"))
	{
		TGuestThread& t = e.iThread[cur];
		if (t.iRequests > 0) { --t.iRequests; return true; }
		BlockCurrent(cpu, h, EThreadWaitAny, 0, 0);
		return true;
	}
	if (!strcmp(name, "EExecRequestSignal")) { SignalThread(e, cur, (int)r[0]); return true; }
	if (!strcmp(name, "EExecThreadRequestSignal")) { SignalThread(e, ThreadIndex(e, r[0]), 1); return true; }
	if (!strcmp(name, "EExecThreadRequestCount")) { const int i = ThreadIndex(e, r[0]); r[0] = i >= 0 ? (uint32_t)e.iThread[i].iRequests : 0; return true; }
	if (!strcmp(name, "EFastExecSetReentryPoint")) { e.iReentry = r[0]; return true; }
	// ---- mutexes, semaphores, condition variables (local ones; the IPC bridge took any native handle) ----
	if (!strcmp(name, "EExecMutexCreate") || !strcmp(name, "EExecSemaphoreCreate") || !strcmp(name, "EExecCondVarCreate"))	// (name, [count,] owner)
	{
		const int wantType = name[5] == 'M' ? 1 : name[5] == 'S' ? 2 : 3;
		char gname[48] = "";
		if (r[0] && GuestDes(cpu, r[0], gname, sizeof(gname)) > 0)		// a global object: its name is unique among those of its type (KErrAlreadyExists)
		{
			for (int k = 0; k < e.iSyncCount; ++k)
				if (e.iSync[k].iType == wantType && !strcasecmp(e.iSync[k].iName, gname)) { r[0] = (uint32_t)-11; return true; }
		}
		const int ni = NewSync(e);
		if (ni < 0) { r[0] = (uint32_t)KErrNoMemory; return true; }
		TSyncObj& s = e.iSync[ni];
		strncpy(s.iName, gname, sizeof(s.iName) - 1);
		s.iType = wantType;
		s.iCount = s.iType == 2 ? (int)r[1] : 0;
		s.iHolder = -1;
		r[0] = KHandleObj + ni;
		return true;
	}
	if (!strcmp(name, "EExecOpenObject") && (r[0] == 4 || r[0] == 5 || r[0] == 16))		// (TObjectType, name, owner): a global semaphore / mutex / condition variable of this process
	{
		char gname[48];
		if (GuestDes(cpu, r[1], gname, sizeof(gname)) > 0)
		{
			const int wantType = r[0] == 5 ? 1 : r[0] == 4 ? 2 : 3;
			for (int k = 0; k < e.iSyncCount; ++k)
				if (e.iSync[k].iType == wantType && e.iSync[k].iName[0] && !strcasecmp(e.iSync[k].iName, gname))
				{
					++e.iSync[k].iRefs;
					r[0] = (uint32_t)(KHandleObj + k);
					return true;
				}
		}
		return false;		// not ours: the native kernel's objects (the bridge)
	}
	if (!strcmp(name, "EExecAfter") || !strcmp(name, "EExecAfterHighRes"))		// (interval us, status): the thread's own timer, ExecHandler::After
	{
		TGuestThread& t = e.iThread[cur];
		t.iAfterStatus = r[1];
		t.iAfterAt = NowUs(h) + ((int32_t)r[0] > 0 ? (uint64_t)(int32_t)r[0] : 0);
		return true;
	}
	if (!strcmp(name, "EExecMutexWait") || !strcmp(name, "EExecSemaphoreWait"))		// (handle, timeout us: 0 forever, -1 poll)
	{
		const bool mutex = name[5] == 'M';
		const int o = SyncIndex(e, r[0], mutex ? 1 : 2);
		const int32_t timeout = (int32_t)r[1];
		if (o < 0) return false;		// a native object (the bridge's)
		if (timeout < -1) { r[0] = (uint32_t)KErrArgument; return true; }
		if (mutex ? TakeMutex(e, o, cur) : (e.iSync[o].iCount > 0 && e.iSync[o].iCount--)) { r[0] = KErrNone; return true; }
		if (timeout == -1) { r[0] = (uint32_t)KErrTimedOut; return true; }
		if (mutex) { e.iThread[cur].iCondCount = 0; e.iThread[cur].iWaitResult = KErrNone; }
		BlockCurrent(cpu, h, mutex ? EThreadWaitMutex : EThreadWaitSem, o, Deadline(h, timeout));
		return true;
	}
	if (!strcmp(name, "EExecMutexSignal"))
	{
		const int o = SyncIndex(e, r[0], 1);
		if (o < 0) return false;
		if (e.iSync[o].iHolder == cur && --e.iSync[o].iCount <= 0) { e.iSync[o].iCount = 0; e.iSync[o].iHolder = -1; HandOffMutex(e, o); }
		return true;
	}
	if (!strcmp(name, "EExecMutexIsHeld")) { const int o = SyncIndex(e, r[0], 1); if (o < 0) return false; r[0] = e.iSync[o].iHolder == cur; return true; }
	if (!strcmp(name, "EExecSemaphoreSignal1") || !strcmp(name, "EExecSemaphoreSignalN"))
	{
		const int o = SyncIndex(e, r[0], 2);
		if (o < 0) return false;
		int n = name[20] == '1' ? 1 : (int)r[1];
		while (n-- > 0)
		{
			const int w = OldestWaiter(e, EThreadWaitSem, o);
			if (w >= 0) Wake(e, w, KErrNone); else ++e.iSync[o].iCount;
		}
		return true;
	}
	if (!strcmp(name, "EExecCondVarWait"))		// (condvar, mutex handle, timeout us)
	{
		const int cv = SyncIndex(e, r[0], 3), m = SyncIndex(e, r[1], 1);
		const int32_t timeout = (int32_t)r[2];
		if (cv < 0 || m < 0) { r[0] = (uint32_t)KErrBadHandle; return true; }
		if (e.iSync[m].iHolder != cur) { r[0] = (uint32_t)KErrGeneral; return true; }		// (the kernel panics a thread that does not hold it)
		if (timeout < -1) { r[0] = (uint32_t)KErrArgument; return true; }
		if (timeout == -1) { r[0] = (uint32_t)KErrTimedOut; return true; }
		TGuestThread& t = e.iThread[cur];
		t.iCondMutex = m;
		t.iCondCount = e.iSync[m].iCount;
		e.iSync[m].iHolder = -1;
		e.iSync[m].iCount = 0;
		HandOffMutex(e, m);
		BlockCurrent(cpu, h, EThreadWaitCond, cv, Deadline(h, timeout));
		return true;
	}
	if (!strcmp(name, "EExecCondVarSignal") || !strcmp(name, "EExecCondVarBroadcast"))
	{
		const int cv = SyncIndex(e, r[0], 3);
		for (int w; cv >= 0 && (w = OldestWaiter(e, EThreadWaitCond, cv)) >= 0; )
		{
			CondToMutex(e, w, KErrNone);
			if (name[12] == 'S') break;
		}
		return true;
	}
	// ---- message queues (RMsgQueue): a real kernel queue of this process (the embedder's iKernel), because a guest hands the queue to a native
	// server in an IPC argument (the A3F DevSound proxy does) and the server writes its events into it; a queue the embedder does not own -- the
	// host tests have no native side -- stays local to the emulation below ----
	if (!strncmp(name, "EExecMsgQueue", 13) && h.iIpc && h.iIpc->iKernel && (!strcmp(name, "EExecMsgQueueCreate") || SyncIndex(e, r[0], 4) < 0))
	{
		if (!strncmp(name + 13, "Notify", 6)) NoteRequest(h, name, r[1]);		// (the native queue completes the status and signals this thread)
		if (h.iIpc->iKernel(h.iIpc->iCtx, name, r, cpu)) return true;
	}
	// ---- message queues (RMsgQueue): local to the process; a named one is only a name for the same thing here ----
	if (!strcmp(name, "EExecMsgQueueCreate"))		// (name or NULL, slots, message length, owner type)
	{
		if ((int32_t)r[1] <= 0 || (int32_t)r[2] < 4 || (r[2] & 3) || r[2] > 256) { r[0] = (uint32_t)KErrArgument; return true; }		// (the kernel panics: KERN-EXEC 49 / 48)
		uint8_t* data = (uint8_t*)calloc(r[1], r[2]);
		const int ni = data ? NewSync(e) : -1;
		if (ni < 0) { free(data); r[0] = (uint32_t)KErrNoMemory; return true; }
		TSyncObj& s = e.iSync[ni];
		s.iType = 4;
		s.iData = data; s.iSlots = (int)r[1]; s.iLen = (int)r[2];
		r[0] = KHandleObj + ni;
		return true;
	}
	if (!strncmp(name, "EExecMsgQueue", 13))
	{
		const int o = SyncIndex(e, r[0], 4);
		if (o < 0) return false;
		TSyncObj& q = e.iSync[o];
		const char* op = name + 13;
		if (!strcmp(op, "Send"))			// (handle, data, length): KErrOverflow when full
		{
			if ((int32_t)r[2] <= 0 || (int)r[2] > q.iLen) { r[0] = (uint32_t)KErrArgument; return true; }
			if (q.iCount == q.iSlots) { r[0] = (uint32_t)KErrOverflow; return true; }
			uint8_t* slot = q.iData + (size_t)((q.iHolder + q.iCount) % q.iSlots) * q.iLen;
			memset(slot, 0, q.iLen);
			for (uint32_t i = 0; i < r[2]; ++i) slot[i] = (uint8_t)cpu.Read8(r[1] + i);
			++q.iCount;
			if (q.iDataStatus) { CompleteFor(cpu, e, q.iDataThread, q.iDataStatus, KErrNone); q.iDataStatus = 0; }
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(op, "Receive"))			// (handle, buffer, length = the message length): KErrUnderflow when empty
		{
			if ((int)r[2] != q.iLen) { r[0] = (uint32_t)KErrArgument; return true; }
			if (!q.iCount) { r[0] = (uint32_t)KErrUnderflow; return true; }
			const uint8_t* slot = q.iData + (size_t)q.iHolder * q.iLen;
			for (int i = 0; i < q.iLen; ++i) cpu.Write8(r[1] + i, slot[i]);
			q.iHolder = (q.iHolder + 1) % q.iSlots;
			--q.iCount;
			if (q.iSpaceStatus) { CompleteFor(cpu, e, q.iSpaceThread, q.iSpaceStatus, KErrNone); q.iSpaceStatus = 0; }
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(op, "NotifySpaceAvailable") || !strcmp(op, "NotifyDataAvailable"))		// (handle, TRequestStatus&)
		{
			const bool space = op[6] == 'S';
			if (space ? q.iCount < q.iSlots : q.iCount > 0) { CompleteFor(cpu, e, cur, r[1], KErrNone); return true; }
			(space ? q.iSpaceStatus : q.iDataStatus) = r[1];
			(space ? q.iSpaceThread : q.iDataThread) = cur;
			return true;
		}
		if (!strcmp(op, "CancelSpaceAvailable") || !strcmp(op, "CancelDataAvailable"))
		{
			const bool space = op[6] == 'S';
			uint32_t& st = space ? q.iSpaceStatus : q.iDataStatus;
			if (st) { CompleteFor(cpu, e, space ? q.iSpaceThread : q.iDataThread, st, KErrCancel); st = 0; }
			return true;
		}
		if (!strcmp(op, "Size")) { r[0] = (uint32_t)q.iLen; return true; }
	}
	// ---- threads ----
	if (!strcmp(name, "EExecThreadCreate")) { r[0] = (uint32_t)CreateThread(cpu, h, r[2]); return true; }	// (name, owner, SThreadCreateInfo8&)
	if (!strcmp(name, "EExecThreadResume")) { const int i = ThreadIndex(e, r[0]); if (i >= 0 && e.iThread[i].iHosted) HostedResume(cpu, h, i); else if (i >= 0 && e.iThread[i].iState == EThreadSuspended) e.iThread[i].iState = EThreadReady; return true; }
	if (!strcmp(name, "EExecThreadSuspend")) { const int i = ThreadIndex(e, r[0]); if (i >= 0 && i != cur && e.iThread[i].iState == EThreadReady) e.iThread[i].iState = EThreadSuspended; return true; }
	if (!strcmp(name, "EExecThreadId")) { const int i = ThreadIndex(e, r[0]); r[0] = i >= 0 ? e.iThread[i].iId : (uint32_t)KErrBadHandle; return true; }
	if (!strcmp(name, "EExecThreadOpenById"))		// (id, owner type)
	{
		int i = 0;
		while (i < e.iThreadCount && (e.iThread[i].iState == EThreadFree || e.iThread[i].iId != r[0])) ++i;
		r[0] = i < e.iThreadCount ? KHandleThread0 + i : (uint32_t)KErrNotFound;
		return true;
	}
	if (!strcmp(name, "EExecThreadExitType")) { const int i = ThreadIndex(e, r[0]); r[0] = i >= 0 && e.iThread[i].iState == EThreadDead ? e.iThread[i].iExitType : 3; return true; }	// EExitPending
	if (!strcmp(name, "EExecThreadExitReason")) { const int i = ThreadIndex(e, r[0]); r[0] = i >= 0 && e.iThread[i].iState == EThreadDead ? e.iThread[i].iExitReason : 0; return true; }
	if (!strcmp(name, "EExecThreadExitCategory")) { const int i = ThreadIndex(e, r[0]); SetDes8(cpu, r[1], i >= 0 ? e.iThread[i].iExitCategory : ""); return true; }
	if (!strcmp(name, "EExecThreadLogon"))		// (thread, TRequestStatus*, rendezvous)
	{
		const int i = ThreadIndex(e, r[0]);
		if (i < 0 || !r[1]) return true;
		TGuestThread& t = e.iThread[i];
		if (t.iState == EThreadDead) { CompleteFor(cpu, e, cur, r[1], t.iExitReason); return true; }
		int k = 0;
		while (k < KMaxLogons && t.iLogonStatus[k]) ++k;
		if (k == KMaxLogons) { CompleteFor(cpu, e, cur, r[1], KErrNoMemory); return true; }
		t.iLogonStatus[k] = r[1];
		t.iLogonOwner[k] = cur;
		t.iLogonRendezvous[k] = r[2] != 0;
		return true;
	}
	if (!strcmp(name, "EExecThreadLogonCancel"))		// (thread, TRequestStatus*, rendezvous): the request completes with KErrNone
	{
		const int i = ThreadIndex(e, r[0]);
		int res = KErrGeneral;
		for (int k = 0; i >= 0 && k < KMaxLogons; ++k)
		{
			TGuestThread& t = e.iThread[i];
			if (t.iLogonStatus[k] != r[1] || t.iLogonOwner[k] != cur || t.iLogonRendezvous[k] != (r[2] != 0)) continue;
			CompleteFor(cpu, e, cur, r[1], KErrNone);
			t.iLogonStatus[k] = 0;
			res = KErrNone;
			break;
		}
		r[0] = (uint32_t)res;
		return true;
	}
	if (!strcmp(name, "EExecThreadRendezvous"))		// (reason): the rendezvous logons on the running thread
	{
		TGuestThread& t = e.iThread[cur];
		for (int k = 0; k < KMaxLogons; ++k)
			if (t.iLogonStatus[k] && t.iLogonRendezvous[k]) { CompleteFor(cpu, e, t.iLogonOwner[k], t.iLogonStatus[k], (int32_t)r[0]); t.iLogonStatus[k] = 0; }
		return true;
	}
	if (!strcmp(name, "EExecThreadKill"))		// (handle, TExitType, reason, const TDesC8* category); the main thread is process-permanent
	{
		const int i = ThreadIndex(e, r[0]);
		if (i <= 0) return false;			// the main thread (or not ours): the process ends (ExecCall)
		if (e.iThread[i].iHosted && e.iThread[i].iState != EThreadDead && h.iIpc->iHostKill) h.iIpc->iHostKill(h.iIpc->iCtx, i, (int)r[2]);
		char cat[17] = "";
		if (r[3]) GuestDes(cpu, r[3], cat, sizeof(cat));
		if (h.iLog)
		{
			char l[120];
			snprintf(l, sizeof(l), "[proteus] guest thread %d ends: type %d %s %d", i, (int)r[1], cat, (int)r[2]);
			h.iLog(l, h.iLogCtx);
		}
		EndThread(cpu, h, i, (int)r[1], (int)r[2], cat);
		if (i == cur) Reschedule(cpu, h);
		return true;
	}
	return false;
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

// ---- servers and sessions inside the guest process ----
// A program may run a server of its own in one of its threads (Let's Golf 2's sound engine: CServer2 / CSession2 in a thread, the game's threads
// its clients). The kernel does this with DServer / DSession / RMessageK (kernel/sipc.cpp); the same process, so every pointer in a message is
// valid for both ends and a descriptor copy is a memory copy. Servers, sessions and messages are guest-side objects here, named by handles in
// ranges of their own (nothing else uses them; the bridge's native sessions are other servers). A session create by name tries these servers
// first; a name that is none of them goes to the native servers as before.
enum { KGMaxSrv = 16, KGMaxSess = 128, KGMaxMsg = 512, KGHandleSrv0 = 0x00F40000, KGHandleSess0 = 0x00F50000, KGHandleMsg0 = 0x00F60000,
	KGConnect = -1, KGDisconnect = -2, KGErrDied = -13, KGErrOverflow = -9, KGErrArgument = -6, KGErrBadHandle = -8, KGErrCancel = -3,
	KGErrServerTerminated = -15, KGErrAlreadyExists = -11, KGErrBadDescriptor = -38, KGErrSessionClosed = -36,
	KGMsgQueued = 1, KGMsgAccepted = 2,		// RMessageK states: delivered to the server's queue / accepted by a Receive
	KGRoleStandalone = 1, KGRoleMaster = 2, KGRoleSlave = 3,		// TIpcSessionRole EServerRole_* (Default = 0 means Standalone)
	KGPanicBadHandle = 0, KGPanicNullCookie = 58, KGPanicInvalidCookieMsg = 60,		// KERN-EXEC (e32kpan.h: EBadHandle, ESessionNullCookie, ESessionInvalidCookieMsg)
	KGObjThread = 0, KGObjChunk = 2, KGObjSemaphore = 4, KGObjMutex = 5, KGObjMsgQueue = 14, KGObjCondVar = 16, KGObjAny = -1,		// TObjectType (u32std.h)
	KGExitPanic = 2 };
struct TGMsg { bool iUsed; int iSess, iFn; uint32_t iArgs[4], iFlags; int iClient; uint32_t iStatus; int iState; uint32_t iSeq; };
struct TGSess { bool iUsed; int iSrv; uint32_t iCookie; bool iClosing, iDiscSent; };
struct TGSrv { bool iUsed; char iName[64]; int iRole, iCreator, iOwner; int iRecvThread; uint32_t iRecvStatus, iRecvMsg; int iQ[KGMaxMsg]; int iQHead, iQCount; };
static uint32_t gGSeq;
static TGMsg gGMsg[KGMaxMsg];
static TGSess gGSess[KGMaxSess];
static TGSrv gGSrv[KGMaxSrv];

static int GSrvOf(uint32_t aHandle) { const uint32_t i = aHandle - KGHandleSrv0; return aHandle >= KGHandleSrv0 && i < KGMaxSrv && gGSrv[i].iUsed ? (int)i : -1; }
static int GSessOf(uint32_t aHandle) { const uint32_t i = aHandle - KGHandleSess0; return aHandle >= KGHandleSess0 && i < KGMaxSess && gGSess[i].iUsed ? (int)i : -1; }
static int GMsgOf(uint32_t aHandle) { const uint32_t i = aHandle - KGHandleMsg0; return aHandle >= KGHandleMsg0 && i < KGMaxMsg && gGMsg[i].iUsed ? (int)i : -1; }

static void GFillMessage(CArmCpu& cpu, uint32_t aBuf, int aMsg)		// RMessage2: iHandle, iFunction, iArgs[4], iSpare1, iSessionPtr, iFlags, iSpare3
{
	const TGMsg& g = gGMsg[aMsg];
	cpu.Write32(aBuf, (uint32_t)(KGHandleMsg0 + aMsg));
	cpu.Write32(aBuf + 4, (uint32_t)g.iFn);
	for (int k = 0; k < 4; ++k) cpu.Write32(aBuf + 8 + 4 * k, g.iArgs[k]);
	cpu.Write32(aBuf + 24, 0);
	cpu.Write32(aBuf + 28, gGSess[g.iSess].iCookie);
	cpu.Write32(aBuf + 32, 0);
	cpu.Write32(aBuf + 36, 0);
}

// DServer::Deliver: the oldest queued message goes to the server's pending Receive
static void GDeliver(CArmCpu& cpu, TExecState& e, int aSrv)
{
	TGSrv& v = gGSrv[aSrv];
	if (!v.iRecvStatus || v.iQCount == 0) return;
	const int m = v.iQ[v.iQHead];
	v.iQHead = (v.iQHead + 1) % KGMaxMsg;
	--v.iQCount;
	gGMsg[m].iState = KGMsgAccepted;
	GFillMessage(cpu, v.iRecvMsg, m);
	const uint32_t st = v.iRecvStatus;
	v.iRecvStatus = 0;
	CompleteFor(cpu, e, v.iRecvThread, st, KErrNone);
}

// DSession::Send: a message to the session's server; aStatus is the client's request (0 = blind, and the disconnect message)
static int GSend(CArmCpu& cpu, TExecState& e, int aSess, int aFn, uint32_t aArgs, uint32_t aStatus, int aClient)
{
	TGSess& s = gGSess[aSess];
	if (s.iSrv < 0 || !gGSrv[s.iSrv].iUsed) return KGErrServerTerminated;
	int m = 0;
	while (m < KGMaxMsg && gGMsg[m].iUsed) ++m;
	if (m == KGMaxMsg) return KErrNoMemory;
	TGMsg& g = gGMsg[m];
	memset(&g, 0, sizeof(g));
	g.iUsed = true; g.iSess = aSess; g.iFn = aFn; g.iClient = aClient; g.iStatus = aStatus; g.iState = KGMsgQueued; g.iSeq = ++gGSeq;
	if (aArgs)
	{
		for (int k = 0; k < 4; ++k) g.iArgs[k] = cpu.Read32(aArgs + 4 * k);
		g.iFlags = cpu.Read32(aArgs + 16);
	}
	if (aStatus) cpu.Write32(aStatus, (uint32_t)KRequestPending);
	TGSrv& v = gGSrv[s.iSrv];
	v.iQ[(v.iQHead + v.iQCount) % KGMaxMsg] = m;
	++v.iQCount;
	GDeliver(cpu, e, s.iSrv);
	return KErrNone;
}

// the descriptor argument aParam of a message: its guest address, the TIpcArgs flag bits (4 = descriptor, 1 = 16-bit, 2 = read-only), or 0
static uint32_t GArgDes(const TGMsg& g, int aParam, uint32_t& aFlags)
{
	aFlags = aParam >= 0 && aParam < 4 ? (g.iFlags >> (3 * aParam)) & 7 : 0;
	return (aFlags & 4) ? g.iArgs[aParam] : 0;
}
// a TDesC's data pointer, length and (a TDes) maximum length, as GuestDes lays them out
static bool GDesInfo(CArmCpu& cpu, uint32_t aDes, uint32_t& aPtr, uint32_t& aLen, uint32_t& aMax, bool& aWritable)
{
	if (!cpu.Valid(aDes, 8)) return false;
	const uint32_t w = cpu.Read32(aDes), type = w >> 28;
	aLen = w & 0x0FFFFFFF;
	aWritable = type >= 2;
	aMax = aLen;
	switch (type)
	{
	case 0: aPtr = aDes + 4; break;
	case 1: aPtr = cpu.Read32(aDes + 4); break;
	case 2: aPtr = cpu.Read32(aDes + 8); aMax = cpu.Read32(aDes + 4); break;
	case 3: aPtr = aDes + 8; aMax = cpu.Read32(aDes + 4); break;
	case 4: aPtr = cpu.Read32(aDes + 8) + 4; aMax = cpu.Read32(aDes + 4); break;
	default: return false;
	}
	return true;
}

static bool GAlive(TExecState& e, int aThread)		// a guest thread that has not died (the kernel: iMState != EDead)
{
	return aThread >= 0 && aThread < e.iThreadCount && e.iThread[aThread].iState != EThreadDead && e.iThread[aThread].iState != EThreadFree;
}

// a message leaves its server's queue (iServerLink.Deque)
static void GUnqueue(TGSrv& v, int aMsg)
{
	int keep[KGMaxMsg], n = 0;
	for (int q = 0; q < v.iQCount; ++q) { const int m = v.iQ[(v.iQHead + q) % KGMaxMsg]; if (m != aMsg) keep[n++] = m; }
	v.iQHead = 0; v.iQCount = n;
	for (int q = 0; q < n; ++q) v.iQ[q] = keep[q];
}

// DSession::Detach: the session's messages that the server was given (queued or accepted) complete with aReason -- unless the client has closed
// the session (IsClosing) or died -- and every message returns to the pool. The disconnect message is only freed.
static void GDetach(CArmCpu& cpu, TExecState& e, int aSess, int aReason)
{
	TGSess& s = gGSess[aSess];
	for (int m = 0; m < KGMaxMsg; ++m)
	{
		TGMsg& g = gGMsg[m];
		if (!g.iUsed || g.iSess != aSess) continue;
		if (g.iState == KGMsgQueued && s.iSrv >= 0) GUnqueue(gGSrv[s.iSrv], m);
		if (g.iFn != KGDisconnect && g.iStatus && !s.iClosing && GAlive(e, g.iClient)) CompleteFor(cpu, e, g.iClient, g.iStatus, aReason);
		g.iUsed = false;
	}
}

// DServer::Close (its last handle): a pending Receive completes with KErrCancel, every session is detached with KErrServerTerminated
static void GFreeServer(CArmCpu& cpu, TExecState& e, int aSrv)
{
	TGSrv& v = gGSrv[aSrv];
	if (v.iRecvStatus && GAlive(e, v.iRecvThread)) CompleteFor(cpu, e, v.iRecvThread, v.iRecvStatus, KGErrCancel);
	v.iRecvStatus = 0;
	for (int k = 0; k < KGMaxSess; ++k)
		if (gGSess[k].iUsed && gGSess[k].iSrv == aSrv)
		{
			GDetach(cpu, e, k, KGErrServerTerminated);
			gGSess[k].iSrv = -1;
			if (gGSess[k].iClosing) memset(&gGSess[k], 0, sizeof(TGSess));		// the client has gone: the session object is deleted with its last access
		}
	memset(&v, 0, sizeof(v));
}

// a guest thread ended: the servers it owns (a named server's handle is thread-owned) are closed with it
static void GThreadDied(CArmCpu& cpu, TExecState& e, int aThread)
{
	for (int k = 0; k < KGMaxSrv; ++k)
		if (gGSrv[k].iUsed && gGSrv[k].iOwner == aThread) GFreeServer(cpu, e, k);
}

// the running thread ends with a KERN-EXEC panic (K::PanicCurrentThread); the main thread's panic ends the process
static void GPanic(CArmCpu& cpu, THle& h, int aReason)
{
	TExecState& e = h.iExec;
	char l[120];
	snprintf(l, sizeof(l), "[proteus] guest thread %d panics: KERN-EXEC %d", e.iCur, aReason);
	if (h.iLog) h.iLog(l, h.iLogCtx);
	if (e.iCur == 0) { cpu.Halt(EExited, aReason); return; }
	EndThread(cpu, h, e.iCur, KGExitPanic, aReason, "KERN-EXEC");
	Reschedule(cpu, h);
}

// ExecHandler::MessageComplete
static void GComplete(CArmCpu& cpu, TExecState& e, int aMsg, int32_t aReason)
{
	TGMsg& g = gGMsg[aMsg];
	const int sess = g.iSess, fn = g.iFn;
	TGSess& s = gGSess[sess];
	if (fn == KGDisconnect)		// DSession::CloseFromDisconnect: the session is detached (nothing completes: the client has closed it) and freed
	{
		GDetach(cpu, e, sess, KGErrSessionClosed);
		g.iUsed = false;
		memset(&s, 0, sizeof(s));
		return;
	}
	const uint32_t status = g.iStatus;
	const int client = g.iClient;
	g.iUsed = false;
	if (fn == KGConnect && aReason != KErrNone) s.iCookie = 0;
	if (!s.iClosing && GAlive(e, client)) CompleteFor(cpu, e, client, status, aReason);
	else if (fn == KGConnect && !s.iCookie && !s.iDiscSent && s.iSrv >= 0 && gGSrv[s.iSrv].iUsed)
	{
		s.iDiscSent = true;		// the connect message of a session closed meanwhile: no user-side session was created, so its disconnect message goes now
		GSend(cpu, e, sess, KGDisconnect, 0, 0, -1);
	}
}

// a handle for MessageOpenObject / MessageCompleteWithHandle: 1 = names an object of aType (KGObjAny = any), 0 = invalid or another type,
// -1 = not a handle of this module (the native kernel's)
static int GObject(TExecState& e, uint32_t aHandle, int aType)
{
	if (aHandle == (uint32_t)KCurrentThreadHandle || (aHandle & 0xFFFF0000u) == KHandleThread0)
		return ThreadIndex(e, aHandle) >= 0 && (aType == KGObjThread || aType == KGObjAny) ? 1 : 0;
	if (ChunkIndex(e, aHandle) >= 0) return aType == KGObjChunk || aType == KGObjAny ? 1 : 0;
	const uint32_t si = aHandle - KHandleObj;
	if (si < (uint32_t)KMaxSync)		// this module's sync object handles: a closed or never made one names nothing
	{
		if (!e.iSync || si >= (uint32_t)e.iSyncCount || !e.iSync[si].iType) return 0;
		const int t = e.iSync[si].iType == 1 ? KGObjMutex : e.iSync[si].iType == 2 ? KGObjSemaphore : e.iSync[si].iType == 3 ? KGObjCondVar : KGObjMsgQueue;
		return aType == t || aType == KGObjAny ? 1 : 0;
	}
	return -1;
}
// DObject::Open + MakeHandle in the process that holds the object already: one more handle on it, naming the same object
static void GOpenHandle(TExecState& e, uint32_t aHandle)
{
	const uint32_t si = aHandle - KHandleObj;
	if (e.iSync && si < (uint32_t)e.iSyncCount && e.iSync[si].iType) ++e.iSync[si].iRefs;
}

// the executive calls of guest servers / sessions / messages; false = not one of ours (the bridge's or another handler's)
static bool GuestIpcExec(CArmCpu& cpu, THle& h, const char* name, uint32_t* r)
{
	TExecState& e = h.iExec;
	if (!e.iThread || strncmp(name, "EExec", 5)) return false;
	const int cur = e.iCur;
	if (!strcmp(name, "EExecServerCreateWithOptions") || !strcmp(name, "EExecServerCreate"))		// (TDesC8* name, mode, role, options)
	{
		char nm[64];
		if (GuestDes(cpu, r[0], nm, sizeof(nm)) < 0) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
		int fr = -1;
		for (int k = 0; k < KGMaxSrv; ++k)
		{
			if (gGSrv[k].iUsed && nm[0] && !strcmp(gGSrv[k].iName, nm)) { r[0] = (uint32_t)KGErrAlreadyExists; return true; }
			if (!gGSrv[k].iUsed && fr < 0) fr = k;
		}
		if (fr < 0) { r[0] = (uint32_t)KErrNoMemory; return true; }
		memset(&gGSrv[fr], 0, sizeof(TGSrv));
		gGSrv[fr].iUsed = true;
		const int mode = (int)r[1], role = !strcmp(name, "EExecServerCreateWithOptions") ? (int)r[2] : 0;
		if (mode < 0 || mode > 2 || role < 0 || role > 3) { gGSrv[fr].iUsed = false; r[0] = (uint32_t)KErrArgument; return true; }		// (ExecHandler::ServerCreateWithOptions)
		gGSrv[fr].iRole = role == 0 ? KGRoleStandalone : role;
		gGSrv[fr].iCreator = cur;
		gGSrv[fr].iOwner = nm[0] ? cur : -1;		// K::MakeHandle(nameLen ? EOwnerThread : EOwnerProcess): a named server dies with its thread
		strncpy(gGSrv[fr].iName, nm, sizeof(gGSrv[fr].iName) - 1);
		Say(h, "[proteus] guest server '%s' created", nm);
		r[0] = (uint32_t)(KGHandleSrv0 + fr);
		return true;
	}
	if (!strcmp(name, "EExecServerReceive"))		// (server handle, TRequestStatus&, RMessage2*)
	{
		const int s = GSrvOf(r[0]);
		if (s < 0) return false;
		TGSrv& v = gGSrv[s];
		if (v.iRecvStatus) { strncpy(cpu.iNote, "ServerReceive with a receive already pending", sizeof(cpu.iNote) - 1); cpu.Halt(EHleStop); return true; }
		cpu.Write32(r[1], (uint32_t)KRequestPending);
		v.iRecvStatus = r[1]; v.iRecvMsg = r[2]; v.iRecvThread = cur;
		GDeliver(cpu, e, s);
		return true;
	}
	if (!strcmp(name, "EExecServerCancel"))
	{
		const int s = GSrvOf(r[0]);
		if (s >= 0 && gGSrv[s].iRecvStatus)
		{
			const uint32_t st = gGSrv[s].iRecvStatus;
			gGSrv[s].iRecvStatus = 0;
			CompleteFor(cpu, e, gGSrv[s].iRecvThread, st, KGErrCancel);
		}
		return s >= 0;
	}
	if (!strcmp(name, "EExecSessionCreate") || !strcmp(name, "EExecSessionCreateFromHandle"))
	{
		int srv = -1;
		if (!strcmp(name, "EExecSessionCreate"))		// (TDesC8& name, slots, policy, type)
		{
			char nm[64];
			if (GuestDes(cpu, r[0], nm, sizeof(nm)) < 0) return false;
			for (int k = 0; k < KGMaxSrv && srv < 0; ++k) if (gGSrv[k].iUsed && !strcmp(gGSrv[k].iName, nm)) srv = k;
		}
		else
			srv = GSrvOf(r[0]);
		if (srv < 0) return false;
		int s = 0;
		while (s < KGMaxSess && gGSess[s].iUsed) ++s;
		if (s == KGMaxSess) { r[0] = (uint32_t)KErrNoMemory; return true; }
		memset(&gGSess[s], 0, sizeof(TGSess));
		gGSess[s].iUsed = true; gGSess[s].iSrv = srv;
		r[0] = (uint32_t)(KGHandleSess0 + s);
		return true;
	}
	const int sess = GSessOf(r[0]);
	if (sess >= 0 && (!strcmp(name, "EExecSessionSendSync") || !strcmp(name, "EExecSessionSend")))		// (handle, function, TIpcArgs*, TRequestStatus*)
	{
		const int rc = GSend(cpu, e, sess, (int)r[1], r[2], r[3], cur);
		if (rc != KErrNone && r[3]) CompleteFor(cpu, e, cur, r[3], rc);
		r[0] = r[3] ? KErrNone : (uint32_t)rc;
		return true;
	}
	if (!strcmp(name, "EExecSessionShare"))		// (TInt& handle, mode)
	{
		if (GSessOf(cpu.Read32(r[0])) < 0) return false;
		r[0] = KErrNone;
		return true;
	}
	if (!strcmp(name, "EExecHandleClose"))
	{
		if (sess >= 0)
		{
			// DSession::Close (the client's last access): the disconnect message is sent unless a connect message is still undecided
			TGSess& s = gGSess[sess];
			if (s.iClosing) { r[0] = KErrNone; return true; }
			s.iClosing = true;
			const bool live = s.iSrv >= 0 && gGSrv[s.iSrv].iUsed;
			int conn = -1;
			for (int m = 0; m < KGMaxMsg; ++m) if (gGMsg[m].iUsed && gGMsg[m].iSess == sess && gGMsg[m].iFn == KGConnect) conn = m;
			if (live && (s.iCookie || conn < 0))
			{
				s.iDiscSent = true;
				GSend(cpu, e, sess, KGDisconnect, 0, 0, -1);
			}
			else if (live && gGMsg[conn].iState == KGMsgQueued)		// not accepted yet: it is withdrawn and completed (the disconnect goes with its completion)
			{
				GUnqueue(gGSrv[s.iSrv], conn);
				GComplete(cpu, e, conn, KGErrSessionClosed);
			}
			else if (!live)
			{
				GDetach(cpu, e, sess, KGErrServerTerminated);
				memset(&s, 0, sizeof(s));
			}
			// else: an accepted connect message: SetSessionPtr / its completion sends the disconnect message
			r[0] = KErrNone;
			return true;
		}
		const int srv = GSrvOf(r[0]);
		if (srv >= 0) { GFreeServer(cpu, e, srv); r[0] = KErrNone; return true; }
		return false;
	}
	// ---- messages (the server's side) ----
	if (strncmp(name, "EExecMessage", 12) && strcmp(name, "EExecSetSessionPtr") && strcmp(name, "EExecTransferSession")) return false;
	const int m = GMsgOf(r[0]);
	if (m < 0) return false;		// (a bridge message handle: none are handed out -- the native servers' messages never reach the guest)
	TGMsg& g = gGMsg[m];
	if (!strcmp(name, "EExecMessageComplete")) { GComplete(cpu, e, m, (int32_t)r[1]); return true; }		// (handle, reason)
	if (!strcmp(name, "EExecSetSessionPtr"))		// (handle, cookie): ExecHandler::SetSessionPtr
	{
		if (!r[1]) { GPanic(cpu, h, KGPanicNullCookie); return true; }
		if (g.iFn != KGConnect) { GPanic(cpu, h, KGPanicInvalidCookieMsg); return true; }
		TGSess& s = gGSess[g.iSess];
		if (s.iCookie) { GPanic(cpu, h, 59); return true; }		// ESessionCookieAlreadySet
		s.iCookie = r[1];
		if (s.iClosing && !s.iDiscSent && s.iSrv >= 0 && gGSrv[s.iSrv].iUsed) { s.iDiscSent = true; GSend(cpu, e, g.iSess, KGDisconnect, 0, 0, -1); }
		return true;
	}
	if (!strcmp(name, "EExecMessageCompleteWithHandle"))		// (msg, handle): ExecHandler::MessageCompleteWithHandle -- a handle on the server's object, made in the client's thread
	{
		const int o = GObject(e, r[1], KGObjAny);
		if (o < 0) { strncpy(cpu.iNote, "MessageCompleteWithHandle of a handle that is not this process's (not emulated)", sizeof(cpu.iNote) - 1); cpu.Halt(EHleStop); return true; }
		if (!o) { GPanic(cpu, h, KGPanicBadHandle); return true; }		// (the kernel looks the handle up unchecked; a bad one faults the server)
		int32_t res;
		if (GAlive(e, g.iClient)) { GOpenHandle(e, r[1]); res = (int32_t)r[1]; }		// pT->MakeHandleAndOpen(EOwnerThread, ...): the handle value
		else res = KGErrDied;		// pT->Open() of a dead thread
		GComplete(cpu, e, m, res);
		return true;
	}
	if (!strcmp(name, "EExecMessageOpenObject"))		// (msg, TObjectType, param, TOwnerType): ExecHandler::MessageOpenObject -- a handle in the server's process on the client's handle argument
	{
		if (r[2] >= 4u) { r[0] = (uint32_t)KGErrArgument; return true; }
		if (((g.iFlags >> (3 * r[2])) & 7) != 1) { r[0] = (uint32_t)KErrBadHandle; return true; }		// TIpcArgs::EHandle
		const uint32_t hnd = g.iArgs[r[2]];
		const int o = GObject(e, hnd, (int)r[1]);
		if (o < 0) { strncpy(cpu.iNote, "MessageOpenObject of a handle that is not this process's (not emulated)", sizeof(cpu.iNote) - 1); cpu.Halt(EHleStop); return true; }
		if (!o) { r[0] = (uint32_t)KErrBadHandle; return true; }
		GOpenHandle(e, hnd);
		r[0] = hnd;		// one process, one handle table: the new handle names the same object
		return true;
	}
	if (!strcmp(name, "EExecMessageKill"))		// (msg, TExitType, reason, const TDesC* category): ExecHandler::MessageKill -- the client thread dies
	{
		const int t = g.iClient;
		if (t < 0) { strncpy(cpu.iNote, "MessageKill of a message without a client thread (disconnect)", sizeof(cpu.iNote) - 1); cpu.Halt(EHleStop); return true; }
		if (!GAlive(e, t)) return true;		// DThread::Die on a dead thread does nothing
		char cat[17] = "";
		if (r[1] == KGExitPanic && r[3]) GuestDes(cpu, r[3], cat, sizeof(cat));
		if (h.iLog)
		{
			char l[120];
			snprintf(l, sizeof(l), "[proteus] guest thread %d killed by its server: type %d %s %d", t, (int)r[1], cat, (int)r[2]);
			h.iLog(l, h.iLogCtx);
		}
		if (t == 0) { cpu.Halt(EExited, (int32_t)r[2]); return true; }		// the main thread ends the process
		if (e.iThread[t].iHosted && h.iIpc && h.iIpc->iHostKill) h.iIpc->iHostKill(h.iIpc->iCtx, t, (int)r[2]);
		EndThread(cpu, h, t, (int)r[1], (int)r[2], cat);
		if (t == cur) Reschedule(cpu, h);
		return true;
	}
	if (!strcmp(name, "EExecTransferSession"))		// (msg, server handle): ExecHandler::TransferSession / DSession::Transfer -- the session moves from a master server to a slave
	{
		if (g.iFn != KGConnect) { GPanic(cpu, h, KGPanicInvalidCookieMsg); return true; }
		TGSess& s = gGSess[g.iSess];
		if (!s.iCookie) { GPanic(cpu, h, KGPanicNullCookie); return true; }
		const int ns = GSrvOf(r[1]);
		if (ns < 0) { GPanic(cpu, h, KGPanicBadHandle); return true; }
		const int os = s.iSrv;
		if (os < 0 || !gGSrv[os].iUsed || gGSrv[os].iRole != KGRoleMaster || gGSrv[ns].iRole != KGRoleSlave		// the current server a master, the new one a slave
			|| !GAlive(e, gGSrv[os].iCreator) || !GAlive(e, gGSrv[ns].iCreator))		// (same process: always; neither server's thread may be dead)
		{ GPanic(cpu, h, KGPanicInvalidCookieMsg); return true; }
		s.iSrv = ns;
		// every message of the session the old server has not accepted goes to the new one, in the order they were sent (the connect message is accepted)
		int mv[KGMaxMsg], n = 0;
		for (int k = 0; k < KGMaxMsg; ++k)
			if (gGMsg[k].iUsed && gGMsg[k].iSess == g.iSess && gGMsg[k].iState == KGMsgQueued) mv[n++] = k;
		for (int a2 = 1; a2 < n; ++a2)
			for (int b2 = a2; b2 > 0 && gGMsg[mv[b2 - 1]].iSeq > gGMsg[mv[b2]].iSeq; --b2) { const int t = mv[b2]; mv[b2] = mv[b2 - 1]; mv[b2 - 1] = t; }
		for (int k = 0; k < n; ++k)
		{
			GUnqueue(gGSrv[os], mv[k]);
			gGSrv[ns].iQ[(gGSrv[ns].iQHead + gGSrv[ns].iQCount) % KGMaxMsg] = mv[k];
			++gGSrv[ns].iQCount;
		}
		GComplete(cpu, e, m, KErrNone);		// "This call also completes the original message with no error"
		GDeliver(cpu, e, ns);
		return true;
	}
	if (!strcmp(name, "EExecMessageConstructFromPtr")) { GFillMessage(cpu, r[1], m); return true; }		// (handle, RMessage2*)
	if (!strcmp(name, "EExecMessageClientStatus")) { r[0] = g.iStatus; return true; }
	if (!strcmp(name, "EExecMessageClient")) { r[0] = g.iClient >= 0 ? (uint32_t)(KHandleThread0 + g.iClient) : (uint32_t)KGErrDied; return true; }	// (handle, owner type): this process's thread
	if (!strcmp(name, "EExecMessageClientProcessFlags") || !strcmp(name, "EExecMessageClientThreadFlags")) { r[0] = 0; return true; }
	if (!strcmp(name, "EExecMessageSetProcessPriority")) { r[0] = KErrNone; return true; }
	if (!strcmp(name, "EExecMessageSecurityInfo"))		// (handle, SSecurityInfo&): the program's own, it is the client
	{
		const TLoaded* mod = h.iProcess ? &h.iProcess->iMod[0] : NULL;
		cpu.Write32(r[1], mod ? mod->iSecureId : 0);
		cpu.Write32(r[1] + 4, mod ? mod->iVendorId : 0);
		cpu.Write32(r[1] + 8, mod ? mod->iCaps[0] : 0);
		cpu.Write32(r[1] + 12, mod ? mod->iCaps[1] : 0);
		return true;
	}
	if (!strcmp(name, "EExecMessageGetDesLength") || !strcmp(name, "EExecMessageGetDesMaxLength"))		// (handle, param)
	{
		uint32_t f, ptr, len, max; bool w;
		if ((int)r[1] < 0 || (int)r[1] >= 4) { r[0] = (uint32_t)KGErrArgument; return true; }
		const uint32_t d = GArgDes(g, (int)r[1], f);
		if (!d || !GDesInfo(cpu, d, ptr, len, max, w)) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
		if (!strcmp(name, "EExecMessageGetDesMaxLength") && !w) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
		r[0] = !strcmp(name, "EExecMessageGetDesLength") ? len : max;
		return true;
	}
	if (!strcmp(name, "EExecMessageIpcCopy"))		// (handle, param, SIpcCopyInfo&, offset): the server reads the client's descriptor, or writes into it
	{
		uint32_t f, ptr, len, max; bool w;
		const int param = (int)r[1];
		const uint32_t info = r[2];
		const int off = (int)r[3];
		const uint32_t lptr = cpu.Read32(info), llen = cpu.Read32(info + 4), fl = cpu.Read32(info + 8);
		const int shift = (fl & 0x80000000u) ? 1 : 0;
		const bool write = (fl & 0x10000000u) != 0;
		if (param < 0 || param >= 4 || off < 0) { r[0] = (uint32_t)KGErrArgument; return true; }
		const uint32_t d = GArgDes(g, param, f);
		if (!d || !GDesInfo(cpu, d, ptr, len, max, w) || (int)(f & 1) != shift) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
		if (!write)
		{
			if ((uint32_t)off > len) { r[0] = (uint32_t)KGErrArgument; return true; }
			const uint32_t n = len - (uint32_t)off;
			const uint32_t c = n < llen ? n : llen;
			if (c && (!cpu.Valid(ptr + ((uint32_t)off << shift), c << shift) || !cpu.Valid(lptr, c << shift))) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
			if (c) memmove((void*)(uintptr_t)lptr, (const void*)(uintptr_t)(ptr + ((uint32_t)off << shift)), c << shift);
			r[0] = n > llen ? (uint32_t)KGErrOverflow : c;		// the number of elements read (the caller sets its length from it)
			return true;
		}
		if (!w || (f & 2)) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
		if ((uint32_t)off + llen > max) { r[0] = (uint32_t)KGErrOverflow; return true; }
		if (llen && (!cpu.Valid(ptr + ((uint32_t)off << shift), llen << shift) || !cpu.Valid(lptr, llen << shift))) { r[0] = (uint32_t)KGErrBadDescriptor; return true; }
		if (llen) memmove((void*)(uintptr_t)(ptr + ((uint32_t)off << shift)), (const void*)(uintptr_t)lptr, llen << shift);
		cpu.Write32(d, (cpu.Read32(d) & 0xF0000000u) | ((uint32_t)off + llen));		// the client's descriptor length
		r[0] = KErrNone;
		return true;
	}
	char l[120];
	snprintf(l, sizeof(l), "guest message call %s is not emulated", name);
	strncpy(cpu.iNote, l, sizeof(cpu.iNote) - 1);
	cpu.Halt(EHleStop);
	return true;
}

static bool DoExecCall(CArmCpu& cpu, THle& h, uint32_t imm);

// trace: an executive call that answers an error the app may turn into its exit (bounded), with the caller
bool ExecCall(CArmCpu& cpu, THle& h, uint32_t imm)
{
	const uint32_t lr = cpu.R[14];
	const int cur = h.iExec.iCur;
	const bool done = DoExecCall(cpu, h, imm);
	static int errLines = 0, nomemLines = 0;
	const int32_t res = (int32_t)cpu.R[0];
	if (h.iLog && h.iProcess && h.iExec.iCur == cur && ((errLines < 100 && (res == -16 || res == -12 || res == -28))	// KErrBadName / KErrPathNotFound / KErrBadDescriptor
		|| (nomemLines < 100 && (res == KErrNoMemory || res == KErrNotSupported || res == -25))))		// KErrNoMemory (what ends a load), KErrNotSupported, KErrEof (-25: what ended Let's Golf 2 and the Calculator): which call, where
	{
		if (res == KErrNoMemory || res == KErrNotSupported || res == -25) ++nomemLines; else ++errLines;
		const uint32_t n = imm & 0x7FFFFF;
		const bool fast = (imm & KFast) != 0;
		const char* name = fast ? (n < sizeof(KFastExecNames) / sizeof(KFastExecNames[0]) ? KFastExecNames[n] : "?")
								: (n < sizeof(KSlowExecNames) / sizeof(KSlowExecNames[0]) ? KSlowExecNames[n] : "?");
		char w[120], l[200];
		Where(*h.iProcess, cpu, lr, w, sizeof(w));
		snprintf(l, sizeof(l), "[proteus] exec %s -> %d (caller %s; r1 %x r2 %x r3 %x)", name, (int)res, w, cpu.R[1], cpu.R[2], cpu.R[3]);
		h.iLog(l, h.iLogCtx);
	}
	return done;
}

static bool DoExecCall(CArmCpu& cpu, THle& h, uint32_t imm)
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
	const bool window = gExecTraceWindow > 0;		// (the bridge opens a window after a connect it wants to see: every exec of the next few)
	if (h.iLog && h.iProcess && (window || (traced < 400 && (!fast || !strcmp(name, "EFastExecWaitForAnyRequest")))))
	{
		if (window) --gExecTraceWindow; else ++traced;
		char w[120], l[200];
		Where(*h.iProcess, cpu, r[14], w, sizeof(w));
		snprintf(l, sizeof(l), "[proteus] exec %s r0 %x r1 %x r2 %x r3 %x lr %s", name, r[0], r[1], r[2], r[3], w);
		h.iLog(l, h.iLogCtx);
		if (fast) Backtrace(cpu, h);
	}
	// trace: every message-queue and process-parameter exec (bounded) -- which handle, which slot; where a queue handle the emulation does not know comes from
	static int qtraced = 0;
	if (h.iLog && h.iProcess && !fast && qtraced < 300 && (!strncmp(name, "EExecMsgQueue", 13) || !strncmp(name, "EExecProcessGet", 15) || !strncmp(name, "EExecProcessData", 16) || !strncmp(name, "EExecProcessSet", 15)))
	{
		++qtraced;
		char w[120], l[200];
		Where(*h.iProcess, cpu, r[14], w, sizeof(w));
		snprintf(l, sizeof(l), "[proteus] qexec %s r0 %x r1 %x r2 %x r3 %x lr %s", name, r[0], r[1], r[2], r[3], w);
		h.iLog(l, h.iLogCtx);
	}
	if (GuestIpcExec(cpu, h, name, r)) return true;
	if (ThreadExec(cpu, h, name, r)) return true;
	if (fast)
	{
		if (!strcmp(name, "EFastExecWaitForAnyRequest"))		// (no thread tables: ExecCall without RunExe)
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
		// a native request (timer, property subscription, change notifier, process logon: TRequestStatus in r1): its completion signal
		// belongs to the guest thread making it
		if (!strcmp(name, "EExecTimerAfter") || !strcmp(name, "EExecTimerAt") || !strcmp(name, "EExecTimerLock") || !strcmp(name, "EExecTimerHighRes")
			|| !strcmp(name, "EExecTimerAgainHighRes") || !strcmp(name, "EExecTimerInactivity") || !strcmp(name, "EExecPropertySubscribe")
			|| !strcmp(name, "EExecChangeNotifierLogon") || !strcmp(name, "EExecProcessLogon"))
			NoteRequest(h, name, r[1]);
		if (h.iIpc && h.iIpc->iKernel && (!strncmp(name, "EExecProperty", 13) || !strcmp(name, "EExecOpenObject") || !strcmp(name, "EExecObjectNext") || !strcmp(name, "EExecPlatSecDiagnostic") || !strncmp(name, "EExecChunk", 10)
			|| !strncmp(name, "EExecMutex", 10) || !strncmp(name, "EExecSemaphore", 14) || !strncmp(name, "EExecChangeNotifier", 19) || !strcmp(name, "EExecResetInactivityTime") || !strcmp(name, "EExecUserInactivityTime") || !strncmp(name, "EExecTimer", 10) || !strncmp(name, "EExecTimeNow", 12) || !strcmp(name, "EExecTickCount") || !strcmp(name, "EExecHandleClose") || !strcmp(name, "EExecHandleCount")
			|| !strcmp(name, "EExecProcessDataParameterLength") || !strcmp(name, "EExecHalFunction") || !strcmp(name, "EExecProcessGetDataParameter") || !strncmp(name, "EExecProcessCommandLine", 23))
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
		if (!strcmp(name, "EExecHandleClose") && ipc && ipc->iOwns(ipc->iCtx, r[0]))
		{
			r[0] = ipc->iClose(ipc->iCtx, r[0]);
			if (e.iThread) HostedSweep(cpu, h);		// (closing a controller's session ends its thread)
			return true;
		}
		{
			// the close of a mutex, semaphore, condition variable or message queue: its handle count (the slot is free again at none; Duplicate is not emulated)
			const uint32_t si = r[0] - KHandleObj;
			const bool sync = e.iSync && si < (uint32_t)e.iSyncCount && e.iSync[si].iType;
			if (sync && !strcmp(name, "EExecHandleClose"))
			{
				if (--e.iSync[si].iRefs <= 0) { free(e.iSync[si].iData); memset(&e.iSync[si], 0, sizeof(TSyncObj)); }
				r[0] = KErrNone;
				return true;
			}
		}
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
			{
				int order[KMaxModules];
				const int nNew = InitOrder(*h.iProcess, mod, e.iLibFirstNew, order);
				for (int k = 0; k < nNew && n < maxEps; ++k)
					if (CallsEntry(cpu, h.iProcess->iMod[order[k]])) cpu.Write32(r[2] + 4 * n++, h.iProcess->iMod[order[k]].iEntry);
			}
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
			if (r[3]) { if (e.iThread) CompleteFor(cpu, e, e.iCur, r[3], res); else { cpu.Write32(r[3], (uint32_t)res); if (ipc) ipc->iSignal(ipc->iCtx, 1); } }
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
		if (!strcmp(name, "EExecSessionCreateFromHandle"))		// (server handle, slots, policy, type): a session with a server the guest holds a handle of
		{
			r[0] = (uint32_t)(ipc && ipc->iSessionFromServer ? ipc->iSessionFromServer(ipc->iCtx, r[0], (int)r[1]) : KErrNotSupported);
			return true;
		}
		if (!strcmp(name, "EExecSessionSendSync") || !strcmp(name, "EExecSessionSend"))	// (handle, function, TIpcArgs*, TRequestStatus*)
		{
			if (!ipc || !ipc->iOwns(ipc->iCtx, r[0])) { r[0] = (uint32_t)KErrBadHandle; return true; }
			bool sync = !strcmp(name, "EExecSessionSendSync");
			// the native thread has one synchronous message slot for all guest threads: while another guest thread's synchronous send is
			// outstanding, this one goes as an asynchronous send (checked before this request's status is set pending: a thread reuses
			// one TRequestStatus for its synchronous sends)
			if (sync && e.iSyncSendStatus && e.iSyncSendStatus != r[3] && cpu.Valid(e.iSyncSendStatus, 4)
				&& cpu.Read32(e.iSyncSendStatus) == 0x80000001u)
				sync = false;
			if (r[3]) cpu.Write32(r[3], 0x80000001u);		// KRequestPending
			NoteRequest(h, name, r[3]);
			r[0] = (uint32_t)ipc->iSend(ipc->iCtx, r[0], (int)r[1], r[2], r[3], sync);
			// a send the bridge completed itself (a synchronous one, a connect): the status is written and the signal belongs to this thread --
			// credited here, not through a native signal that would have to be matched with its status later
			if (ipc->iSyncDone && ipc->iSyncDone(ipc->iCtx) && r[3])
			{
				if (e.iThread) CompleteFor(cpu, e, e.iCur, r[3], (int32_t)cpu.Read32(r[3]));
				else ipc->iSignal(ipc->iCtx, 1);
			}
			if (sync) e.iSyncSendStatus = r[3];
			return true;
		}
		if (!strcmp(name, "EExecSessionShare"))		// (TInt& handle, mode)
		{
			const uint32_t hnd = cpu.Read32(r[0]);
			r[0] = ipc && ipc->iOwns(ipc->iCtx, hnd) ? (uint32_t)ipc->iShare(ipc->iCtx, hnd, (int)r[1]) : (uint32_t)KErrBadHandle;
			return true;
		}
		if (!strcmp(name, "EExecProcessCommandLineLength")) { r[0] = 0; return true; }
		if (!strcmp(name, "EExecStaticCallsDone")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecUserThreadExiting"))
		{
			if ((int)r[0] != 0 && h.iLog)		// User::Exit(reason): who called it (an app's own exit on an error is otherwise silent)
			{
				char l[80];
				snprintf(l, sizeof(l), "[proteus] User::Exit(%d) from the guest:", (int)r[0]);
				h.iLog(l, h.iLogCtx);
				Backtrace(cpu, h);
				// the raw stack as well: the heuristic scan above skips what is not in a module's code range
				if (h.iProcess)
				{
					char w[120];
					snprintf(l, sizeof(l), "[proteus]   sp %08x pc %08x", cpu.R[13], cpu.iLastPc);
					h.iLog(l, h.iLogCtx);
					for (int i = 0; i < 64 && cpu.Valid(cpu.R[13] + i * 4, 4); ++i)
					{
						const uint32_t v = cpu.Read32(cpu.R[13] + i * 4);
						for (int m = 0; m < h.iProcess->iCount; ++m)
							if ((v & ~1u) >= h.iProcess->iMod[m].iLoadAddr && (v & ~1u) < h.iProcess->iMod[m].iEnd)
							{
								Where(*h.iProcess, cpu, v & ~1u, w, sizeof(w));
								snprintf(l, sizeof(l), "[proteus]   sp+%x %08x %s", i * 4, v, w);
								h.iLog(l, h.iLogCtx);
								break;
							}
					}
				}
			}
			r[0] = KErrNone;
			return true;
		}
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
		if (!strcmp(name, "EExecProcessId")) { r[0] = 0x100; return true; }
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
			// SSecurityInfo: iSecureId, iVendorId, iCaps (2 words). The guest's own process/thread: what its EXE header declares, capped at
			// the handler's (All -TCB) -- the guest's own view (RProcess::HasCapability ...); a server still checks the handler process.
			// The creator: unchanged (the UID3 with the handler's capabilities; the real creator is not known here)
			const bool creator = !strcmp(name, "EExecCreatorSecurityInfo");
			const uint32_t si = creator ? r[0] : r[1];
			const TLoaded* x = h.iImage;
			if (creator || !x)
			{
				cpu.Write32(si, x ? x->iUid3 : 0); cpu.Write32(si + 4, 0); cpu.Write32(si + 8, 0xFFFFFFFEu); cpu.Write32(si + 12, 0x7FFFFFFFu);
			}
			else
			{
				cpu.Write32(si, x->iSecureId); cpu.Write32(si + 4, x->iVendorId);
				cpu.Write32(si + 8, x->iCaps[0] & 0xFFFFFFFEu); cpu.Write32(si + 12, x->iCaps[1] & 0x7FFFFFFFu);
			}
			r[0] = KErrNone;
			return true;
		}
		if (!strcmp(name, "EExecProcessPriority")) { r[0] = 350; return true; }		// EPriorityForeground
		if (!strcmp(name, "EExecThreadPriority")) { r[0] = 0; return true; }			// EPriorityNormal
		if (!strcmp(name, "EExecThreadProcessPriority")) { r[0] = 350; return true; }
		if (!strcmp(name, "EExecThreadSetPriority") || !strcmp(name, "EExecThreadSetProcessPriority"))		// trace: who asks for what (honoured by nobody yet)
		{
			static int n = 0;
			if (h.iLog && n < 60)
			{
				++n;
				char l[96];
				snprintf(l, sizeof(l), "[proteus] %s thread %d (cur %d) priority %d", name, ThreadIndex(h.iExec, r[0]), h.iExec.iCur, (int)r[1]);
				h.iLog(l, h.iLogCtx);
			}
		}
		if (!strcmp(name, "EExecProcessSetPriority") || !strcmp(name, "EExecThreadSetPriority") || !strcmp(name, "EExecThreadSetProcessPriority")
			|| !strcmp(name, "EExecProcessSetFlags") || !strcmp(name, "EExecThreadSetFlags") || !strcmp(name, "EExecFsRegisterThread")
			|| !strcmp(name, "EExecWsRegisterThread") || !strcmp(name, "EExecProcessRendezvous")
			|| !strcmp(name, "EExecThreadRename") || !strcmp(name, "EExecProcessRename")) { r[0] = KErrNone; return true; }
		if (!strcmp(name, "EExecProcessFlags") || !strcmp(name, "EExecThreadFlags")) { r[0] = 0; return true; }
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
		if (!strcmp(name, "EExecThreadProcess") || !strcmp(name, "EExecThreadAsProcess")) { r[0] = 0xFFFF8000u; return true; }	// KCurrentProcessHandle
		if (!strcmp(name, "EExecProcessGetMemoryInfo")) { r[0] = (uint32_t)KErrNotSupported; return true; }
		if (!strcmp(name, "EExecProcessExitType")) { r[0] = 3; return true; }		// EExitPending (TExitType: Kill, Terminate, Panic, Pending)
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
		// (TInt& numEps, TLinAddr* eps): the process's entry points, each DLL after the DLLs it imports, the EXE's last (scodeseg.cpp
		// ExecHandler::StaticCallList) -- the real euser calls them from User::InitProcess, after the main thread's heap exists
		if (!strcmp(name, "EExecStaticCallList") && h.iProcess && h.iProcess->iRealEuser)
		{
			const int maxEps = (int)cpu.Read32(r[0]);
			int order[KMaxModules];
			const int nInit = InitOrder(*h.iProcess, 0, 1, order);
			int n = 0;
			for (int k = 0; k < nInit; ++k)
				if (CallsEntry(cpu, h.iProcess->iMod[order[k]]))
				{
					if (n < maxEps) cpu.Write32(r[1] + 4 * n, h.iProcess->iMod[order[k]].iEntry);
					++n;
				}
			if (n < maxEps) cpu.Write32(r[1] + 4 * n, h.iProcess->iMod[0].iEntry);
			++n;
			cpu.Write32(r[0], (uint32_t)n);
			r[0] = n > maxEps ? (uint32_t)-1 : KErrNone;
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

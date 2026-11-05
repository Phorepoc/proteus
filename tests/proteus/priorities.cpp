// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// tests/proteus/priorities.cpp -- self-checking test of the guest thread priorities of the Proteus scheduler (os/src/proteus/proteusexec.cpp).
// Like guest_servers.cpp it includes the executive source itself and drives the executive calls the way ARM code does, as several guest threads.
// What the 2010 kernel does (kernel/eka/kernel/sthread.cpp, sprocess.cpp, sexec.cpp) is checked:
//   the table that maps process + thread priority to the absolute priority (CalcDefaultThreadPriority), RThread::SetPriority /
//   RProcess::SetPriority and the getters, the run order (highest absolute priority first), a woken higher priority thread taking the processor at
//   the next scheduling point, round robin among equals, a process priority change that reorders threads, and the panics (EBadPriority).
// Build + run: bash tests/proteus/run.sh   (exit code 0 = every check passed)
#include "../../os/src/proteus/proteusexec.cpp"
#include <stdio.h>
#include <vector>

using namespace proteus;

static int gFail, gChecks;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFail; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<uint8_t> gMem;
static CArmCpu* gCpu;
static THle* gHle;
static void Log(const char*, void*) {}

enum { KBase = 0x00400000, KThreads = 4 };
enum { PLess = -10, PNormal = 0, PMore = 10, PMuchMore = 20, PRealTime = 30 };			// TThreadPriority
enum { PrBackground = 250, PrForeground = 350, PrHigh = 450, PrWindowServer = 650 };		// TProcessPriority

static uint32_t Th(int t) { return KHandleThread0 + t; }
static uint32_t gR0;
static void Exec(int t, const char* name, uint32_t a = 0, uint32_t b = 0)
{
	gHle->iExec.iCur = t;
	uint32_t r[4] = { a, b, 0, 0 };
	if (!ThreadExec(*gCpu, *gHle, name, r)) { printf("  FAIL: %s was not handled\n", name); ++gFail; }
	gR0 = r[0];
}
static void SetPri(int t, int pri) { Exec(t, "EExecThreadSetPriority", Th(t), (uint32_t)pri); }
static int Cur() { return gHle->iExec.iCur; }
static int Abs(int t) { return gHle->iExec.iThread[t].iAbsPri; }
// the running thread blocks in WaitForAnyRequest (nothing signalled): the scheduler picks the next
static void Block(int t) { Exec(t, "EFastExecWaitForAnyRequest"); }
static void Wake(int t) { Exec(Cur(), "EExecThreadRequestSignal", Th(t)); }

// KThreads ready threads of the main process (Foreground), all at Normal; thread 0 is running
static void Setup()
{
	static CArmCpu cpu;
	static THle hle;
	gMem.assign(1 << 16, 0);
	cpu.iHost = gMem.data();
	cpu.iBase = KBase;
	cpu.iSize = (uint32_t)gMem.size();
	memset(&hle, 0, sizeof(hle));
	gCpu = &cpu; gHle = &hle;
	hle.iLog = Log;
	TExecState& e = hle.iExec;
	e.iThread = (TGuestThread*)calloc(KMaxThreads, sizeof(TGuestThread));
	e.iThreadCount = KThreads;
	for (int i = 0; i < KThreads; ++i) { e.iThread[i].iState = EThreadReady; e.iThread[i].iId = 100 + i; e.iThread[i].iStackChunk = -1; }
	e.iCur = 0;
	e.iSync = (TSyncObj*)calloc(KSyncInit, sizeof(TSyncObj));
	e.iSyncCap = KSyncInit;
	cpu.iHalt = ERunning;
	InitPriorities(hle);		// process priority Foreground (no EXE header here), thread 0 Normal
	for (int i = 1; i < KThreads; ++i) e.iThread[i].iThreadPri = -5;
	RecalcPriorities(hle);
}

static void TestTable()
{
	printf("the priority table (sthread.cpp:429, CalcDefaultThreadPriority)\n");
	// rows: process priority 0..7; columns: Idle MuchLess Less Normal More MuchMore RealTime (thread priority -8..-2)
	CHECK(CalcAbsPriority(-5, 2, true) == 12);		// Foreground, Normal: the usual application thread
	CHECK(CalcAbsPriority(-5, 1, true) == 7);		// Background, Normal
	CHECK(CalcAbsPriority(-5, 0, true) == 3);		// Low, Normal
	CHECK(CalcAbsPriority(-8, 0, true) == 1);		// Low, Idle
	CHECK(CalcAbsPriority(-6, 2, true) == 11 && CalcAbsPriority(-4, 2, true) == 13 && CalcAbsPriority(-3, 2, true) == 14);
	CHECK(CalcAbsPriority(-2, 2, true) == 22);		// Foreground, RealTime
	CHECK(CalcAbsPriority(-5, 3, true) == 19);		// High, Normal
	CHECK(CalcAbsPriority(-5, 4, true) == 21 && CalcAbsPriority(-5, 5, true) == 21 && CalcAbsPriority(-5, 6, true) == 21);	// system servers
	CHECK(CalcAbsPriority(-4, 4, true) == 24 && CalcAbsPriority(-2, 4, true) == 28);
	CHECK(CalcAbsPriority(-5, 7, true) == 28 && CalcAbsPriority(-2, 7, true) == 31);		// RealTimeServer
	CHECK(CalcAbsPriority(-1, 2, true) == 0);		// column 7 of the table is unused: 0
	CHECK(CalcAbsPriority(-20, 2, true) == 3);		// below -8 is clamped to Idle
	// without ProtServ the result is capped to KPrioritySystemServerMore (24): "thread priority capped to below realtime range"
	CHECK(CalcAbsPriority(-2, 4, false) == 24 && CalcAbsPriority(-5, 7, false) == 24 && CalcAbsPriority(-2, 7, false) == 24);
	CHECK(CalcAbsPriority(-2, 2, false) == 22);		// under the cap: unchanged
	// an absolute thread priority (>= 0) ignores the process; 63 is the top
	CHECK(CalcAbsPriority(15, 0, false) == 15 && CalcAbsPriority(31, 7, false) == 31 && CalcAbsPriority(99, 2, true) == 63);
	// TThreadPriority -> TThrdPriority (DThread::SetPriority, sthread.cpp:1009)
	int tp;
	CHECK(ThreadPriKernel(PLess, tp) && tp == -6);
	CHECK(ThreadPriKernel(PNormal, tp) && tp == -5);
	CHECK(ThreadPriKernel(PRealTime, tp) && tp == -2);
	CHECK(ThreadPriKernel(100, tp) && tp == 1);		// EPriorityAbsoluteVeryLow
	CHECK(ThreadPriKernel(350, tp) && tp == 12);		// AbsoluteForegroundNormal
	CHECK(ThreadPriKernel(500, tp) && tp == 23);		// AbsoluteHigh
	CHECK(ThreadPriKernel(810, tp) && tp == 24 && ThreadPriKernel(880, tp) && tp == 31);	// AbsoluteRealTime1..8
	CHECK(!ThreadPriKernel(-30, tp) && !ThreadPriKernel(5, tp) && !ThreadPriKernel(890, tp) && !ThreadPriKernel(811, tp));	// EPriorityNull etc: EBadPriority
	// and back (ConvertThreadPriority, sexec.cpp:367)
	CHECK(ThreadPriUser(-7) == -20 && ThreadPriUser(-5) == 0 && ThreadPriUser(-2) == 30 && ThreadPriUser(12) == 350 && ThreadPriUser(31) == 880);
	// TProcessPriority <-> TProcPriority (sprocess.cpp:299)
	CHECK(ProcPriKernel(150) == 0 && ProcPriKernel(350) == 2 && ProcPriKernel(650) == 4 && ProcPriKernel(750) == 5 && ProcPriKernel(950) == 6 && ProcPriKernel(850) == 7);
	CHECK(ProcPriKernel(0) == -1 && ProcPriKernel(351) == -1);
	// the capability: a process whose EXE header holds ProtServ (bit 10 of the first capability word) is not capped
	static TProcess proc;
	proc.iCount = 1;
	Setup();
	gHle->iProcess = &proc;
	gHle->iExec.iProcPri = 4;
	SetPri(1, PRealTime);
	CHECK(Abs(1) == 24);
	proc.iMod[0].iCaps[0] = 1u << 10;
	RecalcPriorities(*gHle);
	CHECK(Abs(1) == 28);
	gHle->iProcess = 0;
}

static void TestExecCalls()
{
	printf("RThread::SetPriority / RProcess::SetPriority and the getters\n");
	Setup();
	CHECK(Abs(0) == 12 && Abs(1) == 12);
	Exec(0, "EExecThreadPriority", Th(1));
	CHECK((int32_t)gR0 == PNormal);
	SetPri(0, PMore);
	CHECK(gR0 == 0 && Abs(0) == 13 && Abs(1) == 12);
	Exec(1, "EExecThreadPriority", (uint32_t)KCurrentThreadHandle);
	CHECK((int32_t)gR0 == PNormal);
	Exec(0, "EExecThreadPriority", Th(0));
	CHECK((int32_t)gR0 == PMore);
	Exec(0, "EExecThreadProcessPriority", Th(0));
	CHECK(gR0 == PrForeground);
	// the process priority changes every thread of it (DProcess::SetPriority: SetThreadPriority on each)
	Exec(0, "EExecProcessSetPriority", 0xFFFF8000u, PrBackground);
	CHECK(gR0 == 0 && Abs(0) == 8 && Abs(1) == 7);
	Exec(1, "EExecThreadSetProcessPriority", Th(1), PrHigh);
	CHECK(Abs(0) == 20 && Abs(1) == 19);
	Exec(0, "EExecProcessPriority", 0xFFFF8000u);
	CHECK(gR0 == PrHigh);
	// an absolute thread priority
	SetPri(2, 350);
	Exec(0, "EExecThreadPriority", Th(2));
	CHECK(gR0 == 350 && Abs(2) == 12);
	// RMessagePtr2::SetProcessPriority: Background and Foreground only (sipc.cpp:2026)
	Exec(0, "EExecMessageSetProcessPriority", 0, PrForeground);
	CHECK(gR0 == 0 && Abs(1) == 12);
	Exec(0, "EExecMessageSetProcessPriority", 0, PrHigh);
	CHECK((int32_t)gR0 == KErrPermissionDenied && Abs(1) == 12);
}

static void TestBadPriority()
{
	printf("an invalid priority panics the caller: KERN-EXEC 14 (EBadPriority)\n");
	Setup();
	SetPri(1, 77);			// not a TThreadPriority
	CHECK(gHle->iExec.iThread[1].iState == EThreadDead && gHle->iExec.iThread[1].iExitReason == 14 && !strcmp(gHle->iExec.iThread[1].iExitCategory, "KERN-EXEC"));
	CHECK(Cur() != 1);
	Setup();
	Exec(2, "EExecProcessSetPriority", 0xFFFF8000u, PrWindowServer);		// processes may only be set to Low .. High (sexec.cpp:412)
	CHECK(gHle->iExec.iThread[2].iState == EThreadDead && gHle->iExec.iThread[2].iExitReason == 14);
	CHECK(gHle->iExec.iProcPri == 2);
}

static void TestRunOrder()
{
	printf("run order: the highest absolute priority first\n");
	Setup();
	TExecState& e = gHle->iExec;
	SetPri(1, PMore);		// 13
	SetPri(2, PLess);		// 11
	SetPri(3, PMuchMore);		// 14
	// thread 0 (12) runs; at the next scheduling point (Preempt) the 14 takes the processor, though 1 is next in line
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 3);
	Preempt(*gCpu, *gHle);							// alone at the top: it keeps running
	CHECK(Cur() == 3);
	Block(3);										// it blocks: 13, then 12, then 11
	CHECK(Cur() == 1 && e.iThread[3].iState == EThreadWaitAny);
	Block(1);
	CHECK(Cur() == 0);
	Block(0);
	CHECK(Cur() == 2);
	// a lower priority thread is not run while a higher one is ready (no starvation hack: this is what the kernel does)
	for (int k = 0; k < 5; ++k) Preempt(*gCpu, *gHle);
	CHECK(Cur() == 2);
	Wake(0);
	CHECK(e.iThread[0].iState == EThreadReady);
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 0);
	for (int k = 0; k < 5; ++k) Preempt(*gCpu, *gHle);
	CHECK(Cur() == 0 && e.iThread[2].iState == EThreadReady);
}

static void TestPreemptOnWake()
{
	printf("a woken higher priority thread preempts; a lower or equal one does not\n");
	Setup();
	TExecState& e = gHle->iExec;
	SetPri(1, PMore);		// 13, blocked
	SetPri(2, PLess);		// 11, ready
	e.iCur = 0;				// 0 (12) runs; 3 (12) is blocked
	e.iThread[1].iState = EThreadWaitAny;
	e.iThread[3].iState = EThreadWaitAny;
	CHECK(!HigherReady(e));
	Wake(2);										// already ready, lower: nothing changes
	Wake(3);										// equal priority (12): no preemption needed
	CHECK(e.iThread[3].iState == EThreadReady && !HigherReady(e));
	Preempt(*gCpu, *gHle);							// round robin to the equal, not to the lower
	CHECK(Cur() == 3);
	e.iCur = 0;
	Wake(1);										// 13 > 12: the running thread is preempted at once
	CHECK(HigherReady(e));
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 1);
	CHECK(!HigherReady(e));
	// a priority change of the running thread to below a ready one is a scheduling point too
	SetPri(1, PLess);								// 11: threads 0 and 3 (12) are ready
	CHECK(Cur() == 1 && HigherReady(e));
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 0 || Cur() == 3);
}

static void TestRoundRobin()
{
	printf("equal priorities share the processor round robin\n");
	Setup();
	TExecState& e = gHle->iExec;
	int order[9];
	for (int k = 0; k < 9; ++k) { Preempt(*gCpu, *gHle); order[k] = Cur(); }
	CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3 && order[3] == 0 && order[4] == 1 && order[8] == 1);
	// a blocked thread is skipped, and comes back in its place when it is woken
	e.iThread[2].iState = EThreadWaitAny;
	e.iCur = 0;
	order[0] = order[1] = order[2] = 0;
	for (int k = 0; k < 3; ++k) { Preempt(*gCpu, *gHle); order[k] = Cur(); }
	CHECK(order[0] == 1 && order[1] == 3 && order[2] == 0);
	Wake(2);
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 1);
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 2);
	// a higher thread in the mix takes every turn; the equals under it do not run until it blocks
	SetPri(3, PMore);
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 3);
	for (int k = 0; k < 4; ++k) { Preempt(*gCpu, *gHle); CHECK(Cur() == 3); }
	Block(3);
	CHECK(Cur() != 3);
}

static void TestProcessChangeReorders()
{
	printf("a process priority change reorders threads of relative and absolute priority\n");
	Setup();
	TExecState& e = gHle->iExec;
	SetPri(1, 250);			// AbsoluteBackgroundNormal = 7 ... still below Normal (12) in a Foreground process
	CHECK(Abs(1) == 7 && Abs(0) == 12);
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 2 || Cur() == 3);				// 1 does not run while 2 and 3 are ready
	Exec(0, "EExecProcessSetPriority", 0xFFFF8000u, 150);		// the process goes to Low: Normal = 3, the absolute 7 now wins
	CHECK(Abs(0) == 3 && Abs(1) == 7);
	Preempt(*gCpu, *gHle);
	CHECK(Cur() == 1);
	Block(1);
	CHECK(Cur() != 1 && e.iThread[Cur()].iAbsPri == 3);
}

int main()
{
	TestTable();
	TestExecCalls();
	TestBadPriority();
	TestRunOrder();
	TestPreemptOnWake();
	TestRoundRobin();
	TestProcessChangeReorders();
	printf("priorities: %d checks, %d failed -> %s\n", gChecks, gFail, gFail ? "FAIL" : "PASS");
	return gFail ? 1 : 0;
}

// proteusgl.cpp (linked for proteusrt.cpp's GL hooks) needs the window-surface hook; a host test has no window
namespace proteus { bool GlWindowSurface(CArmCpu&, THle&, uint32_t) { return false; } }

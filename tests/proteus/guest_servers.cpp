// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// tests/proteus/guest_servers.cpp -- self-checking test of the guest-side servers of the Proteus executive (os/src/proteus/proteusexec.cpp).
// It includes the executive source itself and drives its executive-call handlers the way ARM code does (the same names, the same argument
// registers, guest memory for descriptors / TIpcArgs / TRequestStatus), as several guest threads: a client, servers, a bystander. Each exec the
// kernel has (kernel/eka/kernel/sipc.cpp, server.cpp) is checked against what that source does:
//   MessageComplete / MessageCompleteWithHandle / MessageKill / MessageOpenObject / TransferSession / SetSessionPtr, a session's disconnect, the
//   death of a server's thread (DServer::Close -> DSession::Detach: KErrServerTerminated) and of a client's thread.
// Build + run: bash tests/proteus/run.sh   (exit code 0 = every check passed)
#include "../../os/src/proteus/proteusexec.cpp"
#include <stdio.h>
#include <vector>
#include <string>

using namespace proteus;

static int gFail, gChecks;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFail; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<uint8_t> gMem;
static CArmCpu* gCpu;
static THle* gHle;
static std::vector<std::string> gLog;
static void Log(const char* l, void*) { gLog.push_back(l); }

enum { KBase = 0x00400000, KPending = (int)0x80000001 };
// the scratch layout of the guest memory
enum { AName = 0x401000, AArgs = 0x401100, AMsg = 0x401200, AStat = 0x401400, ACat = 0x401800 };

static void W(uint32_t a, uint32_t v) { gCpu->Write32(a, v); }
static uint32_t R(uint32_t a) { return gCpu->Read32(a); }
static int32_t Stat(int k) { return (int32_t)R(AStat + 4 * k); }
static uint32_t S(int k) { return AStat + 4 * k; }		// the address of the k-th TRequestStatus
static void PutName(uint32_t a, const char* n) { W(a, (uint32_t)strlen(n)); for (size_t i = 0; i <= strlen(n); ++i) gCpu->Write8(a + 4 + (uint32_t)i, (uint8_t)n[i]); }
static void PutArgs(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t flags)
{
	W(AArgs, a1); W(AArgs + 4, a2); W(AArgs + 8, a3); W(AArgs + 12, a4); W(AArgs + 16, flags);
}

static const int KThreads = 6;
// the guest thread running the next exec (the kernel's TheCurrentThread)
static void As(int t) { gHle->iExec.iCur = t; }
static int Reqs(int t) { return gHle->iExec.iThread[t].iRequests; }

// one executive call: false = a handler declined it (not one of ours); r0 holds the result
static uint32_t gR0;
static bool Exec(int t, const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0)
{
	As(t);
	uint32_t r[4] = { a, b, c, d };
	const bool ours = GuestIpcExec(*gCpu, *gHle, name, r) || ThreadExec(*gCpu, *gHle, name, r);
	gR0 = r[0];
	return ours;
}
static uint32_t Call(int t, const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0)
{
	if (!Exec(t, name, a, b, c, d)) { printf("  FAIL: %s was not handled\n", name); ++gFail; }
	return gR0;
}

static uint32_t NewServer(int t, const char* name, int role)
{
	PutName(AName, name);
	return Call(t, "EExecServerCreateWithOptions", AName, 0, (uint32_t)role, 0);
}
static uint32_t NewSession(int t, const char* name)
{
	PutName(AName, name);
	return Call(t, "EExecSessionCreate", AName, 4, 0, 0);
}
// client t sends function aFn on session aSess (blind if aStat = 0); returns the exec result
static uint32_t Send(int t, uint32_t aSess, int aFn, uint32_t aStat, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t flags = 0, uint32_t a3 = 0)
{
	PutArgs(a1, a2, a3, 0, flags);
	return Call(t, "EExecSessionSend", aSess, (uint32_t)aFn, AArgs, aStat);
}
// server t receives into AMsg + 64 * k; the message handle is read back from the RMessage2
static uint32_t Receive(int t, uint32_t aSrv, uint32_t aStat, uint32_t aMsg) { return Call(t, "EExecServerReceive", aSrv, aStat, aMsg); }

static void Setup()
{
	static CArmCpu cpu;
	static THle hle;
	gMem.assign(1 << 20, 0);
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
	memset(gGMsg, 0, sizeof(gGMsg)); memset(gGSess, 0, sizeof(gGSess)); memset(gGSrv, 0, sizeof(gGSrv)); gGSeq = 0;
	memset(gMem.data(), 0, gMem.size());
	cpu.iHalt = ERunning;
}

// ---- the tests: thread 1 = client, 2 = server, 3 = second server, 4 = bystander, 0 = main ----

static void TestBasicAndComplete()
{
	printf("basic: connect / receive / SetSessionPtr / complete\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.basic", 1);
	CHECK((int32_t)srv > 0);
	CHECK((int32_t)NewServer(3, "protsrv.basic", 1) == -11);		// KErrAlreadyExists
	const uint32_t ses = NewSession(1, "protsrv.basic");
	CHECK(ses >= KGHandleSess0);
	Send(1, ses, -1, S(0));
	CHECK(Stat(0) == KPending);
	Receive(2, srv, S(1), AMsg);
	CHECK(Stat(1) == 0 && (int32_t)R(AMsg + 4) == -1);			// the Receive completed: a connect message
	CHECK(Reqs(2) == 1);
	const uint32_t msg = R(AMsg);
	Call(2, "EExecSetSessionPtr", msg, 0x1234);
	Call(2, "EExecMessageComplete", msg, 0);
	CHECK(Stat(0) == 0 && Reqs(1) == 1);
	// a function with arguments: the client's TIpcArgs reach the server
	Send(1, ses, 9, S(2), 11, 22);
	Receive(2, srv, S(3), AMsg);
	CHECK(R(AMsg + 4) == 9 && R(AMsg + 8) == 11 && R(AMsg + 12) == 22 && R(AMsg + 28) == 0x1234);		// iFunction, iArgs, iSessionPtr (the cookie)
	Call(2, "EExecMessageComplete", R(AMsg), 5);
	CHECK(Stat(2) == 5);
}

static void TestCompleteWithHandle()
{
	printf("MessageCompleteWithHandle (sipc.cpp:2086)\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.cwh", 1), ses = NewSession(1, "protsrv.cwh");
	// the server's object: a semaphore (one handle)
	const uint32_t sem = Call(2, "EExecSemaphoreCreate", 0, 0, 0);
	CHECK(gHle->iExec.iSync[sem - KHandleObj].iRefs == 1);
	Send(1, ses, 3, S(0));
	Receive(2, srv, S(1), AMsg);
	Call(2, "EExecMessageCompleteWithHandle", R(AMsg), sem);
	CHECK(Stat(0) == (int32_t)sem);					// the client's request completes with the new handle
	CHECK(gHle->iExec.iSync[sem - KHandleObj].iRefs == 2);		// a handle was made on the same object (pO->CheckedOpen / MakeHandleAndOpen)
	CHECK(!gGMsg[R(AMsg) - KGHandleMsg0].iUsed);			// and the message was completed
	// the handle works for the client: it signals the object
	Call(1, "EExecSemaphoreSignal1", (uint32_t)Stat(0));
	CHECK(gHle->iExec.iSync[sem - KHandleObj].iCount == 1);
	// a client that has died: Open() fails, the message completes with KErrDied and nobody is signalled
	Send(1, ses, 3, S(2));
	Receive(2, srv, S(3), AMsg);
	const int reqs = Reqs(1);
	gHle->iExec.iThread[1].iState = EThreadDead;
	Call(2, "EExecMessageCompleteWithHandle", R(AMsg), sem);
	CHECK(Stat(2) == KPending && Reqs(1) == reqs);
	CHECK(gHle->iExec.iSync[sem - KHandleObj].iRefs == 2);
	gHle->iExec.iThread[1].iState = EThreadReady;
	// a handle that names nothing (a closed slot of this process's table): the server is panicked (EBadHandle)
	Send(1, ses, 3, S(4));
	Receive(2, srv, S(5), AMsg);
	const uint32_t dead = Call(2, "EExecSemaphoreCreate", 0, 0, 0);
	gHle->iExec.iSync[dead - KHandleObj].iType = 0;
	Call(2, "EExecMessageCompleteWithHandle", R(AMsg), dead);
	CHECK(gHle->iExec.iThread[2].iState == EThreadDead && gHle->iExec.iThread[2].iExitType == 2 && gHle->iExec.iThread[2].iExitReason == 0);
	CHECK(!strcmp(gHle->iExec.iThread[2].iExitCategory, "KERN-EXEC"));
}

static void TestOpenObject()
{
	printf("MessageOpenObject (sipc.cpp:2055)\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.oo", 1), ses = NewSession(1, "protsrv.oo");
	const uint32_t sem = Call(1, "EExecSemaphoreCreate", 0, 0, 0), mtx = Call(1, "EExecMutexCreate", 0, 0, 0);
	// arg 0 = the semaphore as a handle (type 1), arg 1 = a descriptor (type 4), arg 2 = the mutex handle
	PutName(AName, "x");
	Send(1, ses, 4, S(0), sem, AName, 1 | (4 << 3) | (1 << 6), mtx);
	Receive(2, srv, S(1), AMsg);
	const uint32_t msg = R(AMsg);
	CHECK(Call(2, "EExecMessageOpenObject", msg, 4, 0, 0) == sem);				// ESemaphore, argument 0: a handle on the same object
	CHECK(gHle->iExec.iSync[sem - KHandleObj].iRefs == 2);
	CHECK((int32_t)Call(2, "EExecMessageOpenObject", msg, 5, 0, 0) == -8);			// EMutex asked for, a semaphore given: KErrBadHandle
	CHECK((int32_t)Call(2, "EExecMessageOpenObject", msg, 4, 1, 0) == -8);			// argument 1 is a descriptor, not a handle
	CHECK((int32_t)Call(2, "EExecMessageOpenObject", msg, 4, 4, 0) == -6);			// argument 4: KErrArgument
	CHECK((int32_t)Call(2, "EExecMessageOpenObject", msg, 4, (uint32_t)-1, 0) == -6);
	CHECK(Call(2, "EExecMessageOpenObject", msg, 5, 2, 0) == mtx);				// the mutex, argument 2
	CHECK(gHle->iExec.iSync[mtx - KHandleObj].iRefs == 2);
	CHECK(Call(2, "EExecMessageOpenObject", msg, (uint32_t)-1, 0, 0) == sem);		// EObjectTypeAny
	// a handle that names nothing (a closed slot of this process's table)
	const uint32_t dead = Call(1, "EExecSemaphoreCreate", 0, 0, 0);
	gHle->iExec.iSync[dead - KHandleObj].iType = 0;
	gGMsg[msg - KGHandleMsg0].iArgs[0] = dead;
	CHECK((int32_t)Call(2, "EExecMessageOpenObject", msg, 4, 0, 0) == -8);
	// a thread handle of the client
	gGMsg[msg - KGHandleMsg0].iArgs[0] = KHandleThread0 + 3;
	CHECK(Call(2, "EExecMessageOpenObject", msg, 0, 0, 0) == KHandleThread0 + 3);		// EThread
}

static void TestNativeHandlesHalt()
{
	printf("handles of the native kernel are not emulated: the execs halt loudly\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.nat", 1), ses = NewSession(1, "protsrv.nat");
	Send(1, ses, 4, S(0), 0x00E50000, 0, 1);
	Receive(2, srv, S(1), AMsg);
	Call(2, "EExecMessageOpenObject", R(AMsg), 4, 0, 0);
	CHECK(gCpu->iHalt == EHleStop && strstr(gCpu->iNote, "MessageOpenObject"));
	Setup();
	const uint32_t srv2 = NewServer(2, "protsrv.nat", 1), ses2 = NewSession(1, "protsrv.nat");
	Send(1, ses2, 4, S(0));
	Receive(2, srv2, S(1), AMsg);
	Call(2, "EExecMessageCompleteWithHandle", R(AMsg), 0x00E50000);
	CHECK(gCpu->iHalt == EHleStop && strstr(gCpu->iNote, "MessageCompleteWithHandle"));
	gCpu->iHalt = ERunning;
}

static void TestKill()
{
	printf("MessageKill (sipc.cpp:2042)\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.kill", 1), ses = NewSession(1, "protsrv.kill");
	TGuestThread& cl = gHle->iExec.iThread[1];
	cl.iLogonStatus[0] = S(7); cl.iLogonOwner[0] = 4;		// thread 4 has logged on to the client
	Send(1, ses, 3, S(0));
	Receive(2, srv, S(1), AMsg);
	PutName(ACat, "BADCLIENT");
	const uint32_t msg = R(AMsg);
	Call(2, "EExecMessageKill", msg, 2, 77, ACat);
	CHECK(cl.iState == EThreadDead && cl.iExitType == 2 && cl.iExitReason == 77 && !strcmp(cl.iExitCategory, "BADCLIENT"));
	CHECK(Stat(7) == 77 && Reqs(4) == 1);				// its logon completed with the reason
	// the server completes the message afterwards: the dead client's request is not completed
	const int reqs = Reqs(1);
	Call(2, "EExecMessageComplete", msg, 0);
	CHECK(Stat(0) == KPending && Reqs(1) == reqs);
	// a Kill (type 0) carries no category
	Setup();
	const uint32_t srv2 = NewServer(2, "protsrv.kill2", 1), ses2 = NewSession(1, "protsrv.kill2");
	Send(1, ses2, 3, S(0));
	Receive(2, srv2, S(1), AMsg);
	Call(2, "EExecMessageKill", R(AMsg), 0, 5, 0);
	CHECK(gHle->iExec.iThread[1].iExitType == 0 && gHle->iExec.iThread[1].iExitReason == 5 && gHle->iExec.iThread[1].iExitCategory[0] == 0);
	// killing a client that is already dead does nothing; a message without a client thread (disconnect) cannot be killed
	Call(2, "EExecMessageKill", R(AMsg), 2, 9, 0);
	CHECK(gHle->iExec.iThread[1].iExitReason == 5);
	// the client is the main thread: the process ends
	Setup();
	const uint32_t srv3 = NewServer(2, "protsrv.kill3", 1), ses3 = NewSession(0, "protsrv.kill3");
	Send(0, ses3, 3, S(0));
	Receive(2, srv3, S(1), AMsg);
	Call(2, "EExecMessageKill", R(AMsg), 1, 3, 0);
	CHECK(gCpu->iHalt == EExited && gCpu->iExitCode == 3);
	gCpu->iHalt = ERunning;
}

static void TestTransfer()
{
	printf("TransferSession (sipc.cpp:1668, DSession::Transfer:1043)\n");
	Setup();
	const uint32_t master = NewServer(2, "protsrv.master", 2), slave = NewServer(3, "protsrv.slave", 3);
	const uint32_t ses = NewSession(1, "protsrv.master");
	Send(1, ses, -1, S(0));					// connect
	Send(1, ses, 7, S(1), 70);					// two requests behind it, before the master has looked at them
	Send(1, ses, 8, S(2), 80);
	Receive(2, master, S(3), AMsg);					// the master accepts the connect message
	CHECK((int32_t)R(AMsg + 4) == -1);
	const uint32_t msg = R(AMsg);
	CHECK(gGSrv[master - KGHandleSrv0].iQCount == 2);
	Call(2, "EExecSetSessionPtr", msg, 0xABCD);
	Call(2, "EExecTransferSession", msg, slave);
	CHECK(Stat(0) == 0);						// the connect message completed with no error
	CHECK(gGSess[ses - KGHandleSess0].iSrv == (int)(slave - KGHandleSrv0));
	CHECK(gGSrv[master - KGHandleSrv0].iQCount == 0 && gGSrv[slave - KGHandleSrv0].iQCount == 2);
	Receive(3, slave, S(4), AMsg);					// the slave gets them in the order they were sent
	CHECK(R(AMsg + 4) == 7 && R(AMsg + 8) == 70 && R(AMsg + 28) == 0xABCD);
	Call(3, "EExecMessageComplete", R(AMsg), 1);
	Receive(3, slave, S(5), AMsg);
	CHECK(R(AMsg + 4) == 8);
	Call(3, "EExecMessageComplete", R(AMsg), 2);
	CHECK(Stat(1) == 1 && Stat(2) == 2);
	// new messages go to the slave; the session's close sends its disconnect message there
	Send(1, ses, 9, S(6));
	CHECK(gGSrv[slave - KGHandleSrv0].iQCount == 1 && gGSrv[master - KGHandleSrv0].iQCount == 0);
	Receive(3, slave, S(8), AMsg);
	Call(3, "EExecMessageComplete", R(AMsg), 0);
	Call(1, "EExecHandleClose", ses);
	Receive(3, slave, S(9), AMsg);
	CHECK((int32_t)R(AMsg + 4) == -2);
	Call(3, "EExecMessageComplete", R(AMsg), 0);
	CHECK(!gGSess[ses - KGHandleSess0].iUsed);

	// a disconnect message already delivered but not accepted moves with the session
	Setup();
	const uint32_t m2 = NewServer(2, "protsrv.m2", 2), s2 = NewServer(3, "protsrv.s2", 3);
	const uint32_t ses2 = NewSession(1, "protsrv.m2");
	Send(1, ses2, -1, S(0));
	Receive(2, m2, S(1), AMsg);
	const uint32_t cmsg = R(AMsg);
	Call(2, "EExecSetSessionPtr", cmsg, 5);
	Call(1, "EExecHandleClose", ses2);				// disconnect queued on the master
	CHECK(gGSrv[m2 - KGHandleSrv0].iQCount == 1);
	Call(2, "EExecTransferSession", cmsg, s2);
	CHECK(gGSrv[m2 - KGHandleSrv0].iQCount == 0 && gGSrv[s2 - KGHandleSrv0].iQCount == 1);

	// the panics (K::PanicCurrentThread): not the connect message, no cookie, wrong roles, a handle that is no server
	Setup();
	const uint32_t ma = NewServer(2, "protsrv.ma", 2), sl = NewServer(3, "protsrv.sl", 3), st = NewServer(4, "protsrv.st", 1);
	const uint32_t sa = NewSession(1, "protsrv.ma");
	Send(1, sa, 5, S(0));
	Receive(2, ma, S(1), AMsg);
	Call(2, "EExecTransferSession", R(AMsg), sl);
	CHECK(gHle->iExec.iThread[2].iState == EThreadDead && gHle->iExec.iThread[2].iExitReason == 60);	// ESessionInvalidCookieMsg
	Setup();
	const uint32_t ma3 = NewServer(2, "protsrv.ma", 2), sl3 = NewServer(3, "protsrv.sl", 3), st3 = NewServer(4, "protsrv.st", 1);
	const uint32_t sb = NewSession(1, "protsrv.ma");
	Send(1, sb, -1, S(0));
	Receive(2, ma3, S(1), AMsg);
	Call(2, "EExecTransferSession", R(AMsg), sl3);			// no cookie yet
	CHECK(gHle->iExec.iThread[2].iExitReason == 58);						// ESessionNullCookie
	Setup();
	const uint32_t ma4 = NewServer(2, "protsrv.ma", 2), sl4 = NewServer(3, "protsrv.sl", 3), st4 = NewServer(4, "protsrv.st", 1);
	const uint32_t sc = NewSession(1, "protsrv.ma");
	Send(1, sc, -1, S(0));
	Receive(2, ma4, S(1), AMsg);
	Call(2, "EExecSetSessionPtr", R(AMsg), 9);
	Call(2, "EExecTransferSession", R(AMsg), st4);			// the target is a standalone server, not a slave
	CHECK(gHle->iExec.iThread[2].iExitReason == 60);
	CHECK(gGSess[sc - KGHandleSess0].iSrv == -1);			// (the panicked thread's server closed with it: the session is detached, not moved)
	Setup();
	const uint32_t ma5 = NewServer(2, "protsrv.ma", 2), sl5 = NewServer(3, "protsrv.sl", 3);
	const uint32_t sd = NewSession(1, "protsrv.ma");
	Send(1, sd, -1, S(0));
	Receive(2, ma5, S(1), AMsg);
	Call(2, "EExecSetSessionPtr", R(AMsg), 9);
	gHle->iExec.iThread[3].iState = EThreadDead;			// the slave's thread is dead: transfers must not involve moribund servers
	Call(2, "EExecTransferSession", R(AMsg), sl5);
	CHECK(gHle->iExec.iThread[2].iExitReason == 60);
	// SetSessionPtr's own panics: null cookie, not a connect message, set twice
	Setup();
	const uint32_t ms = NewServer(2, "protsrv.sp", 1), ss = NewSession(1, "protsrv.sp");
	Send(1, ss, -1, S(0));
	Receive(2, ms, S(1), AMsg);
	const uint32_t cm = R(AMsg);
	Call(2, "EExecSetSessionPtr", cm, 0);
	CHECK(gHle->iExec.iThread[2].iExitReason == 58);
	Setup();
	const uint32_t ms2 = NewServer(2, "protsrv.sp", 1), ss2 = NewSession(1, "protsrv.sp");
	Send(1, ss2, -1, S(0));
	Receive(2, ms2, S(1), AMsg);
	Call(2, "EExecSetSessionPtr", R(AMsg), 1);
	Call(2, "EExecSetSessionPtr", R(AMsg), 2);
	CHECK(gHle->iExec.iThread[2].iExitReason == 59);			// ESessionCookieAlreadySet
}

static void TestServerDeath()
{
	printf("a server's thread dies (DServer::Close -> DSession::Detach, sipc.cpp:723, 1117)\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.die", 1);
	const uint32_t ses = NewSession(1, "protsrv.die"), ses2 = NewSession(4, "protsrv.die");
	Send(1, ses, -1, S(0));
	Receive(2, srv, S(1), AMsg);					// connect accepted, cookie set, completed
	Call(2, "EExecSetSessionPtr", R(AMsg), 1);
	Call(2, "EExecMessageComplete", R(AMsg), 0);
	Send(1, ses, 5, S(2));						// accepted by the server, not completed
	Receive(2, srv, S(3), AMsg);
	CHECK(R(AMsg + 4) == 5);
	Send(1, ses, 6, S(4));						// queued, never received
	Send(4, ses2, -1, S(5));						// a second session's connect, queued
	Receive(2, srv, S(6), AMsg + 64);					// the server's next Receive: gets message 6
	Send(1, ses, 7, S(7));						// queued behind it
	Receive(2, srv, S(8), AMsg + 128);					// accepted: 7 ...
	Send(1, ses, 8, S(9));						// ... queued: 8
	CHECK(Stat(8) == 0);
	// a Receive pending on the server at its death completes with KErrCancel
	const uint32_t srvB = NewServer(3, "protsrv.die.b", 1);
	Receive(3, srvB, S(10), AMsg + 192);
	CHECK(Stat(10) == KPending);
	const int r1 = Reqs(1), r4 = Reqs(4);
	// thread 2 dies (ExecHandler::ThreadKill): its (named) server is closed with it
	Call(2, "EExecThreadKill", KHandleThread0 + 2, 0, 0, 0);
	CHECK(gHle->iExec.iThread[2].iState == EThreadDead);
	CHECK(!gGSrv[srv - KGHandleSrv0].iUsed);
	CHECK(Stat(7) == -15 && Stat(9) == -15 && Stat(5) == -15);		// accepted, queued and the other session's: all KErrServerTerminated
	CHECK(Reqs(1) == r1 + 4 && Reqs(4) == r4 + 1);				// the client's statuses 2, 4, 7, 9 and the other client's 5
	CHECK(Stat(4) == -15);
	CHECK(Stat(10) == KPending && gGSrv[srvB - KGHandleSrv0].iUsed);	// another thread's server is unaffected
	// the session survives the server as a handle: sending now says the server is gone, and closing it frees it
	CHECK((int32_t)Send(1, ses, 10, 0) == -15);
	const int r1b = Reqs(1);
	CHECK((int32_t)Send(1, ses, 10, S(11)) == 0 && Stat(11) == -15 && Reqs(1) == r1b + 1);	// SessionSend with a status: completed with the error
	Call(1, "EExecHandleClose", ses);
	CHECK(!gGSess[ses - KGHandleSess0].iUsed);
	Call(4, "EExecHandleClose", ses2);
	CHECK(!gGSess[ses2 - KGHandleSess0].iUsed);

	// closing the server's handle: its pending Receive is cancelled (the owner still runs), the same for every session
	Setup();
	const uint32_t sv = NewServer(2, "protsrv.close", 1), sn = NewSession(1, "protsrv.close");
	Receive(2, sv, S(0), AMsg);
	Send(1, sn, -1, S(1));						// delivered straight into the pending Receive: accepted
	CHECK(Stat(0) == 0);
	Receive(2, sv, S(2), AMsg + 64);
	Call(2, "EExecHandleClose", sv);
	CHECK(Stat(2) == -3);						// KErrCancel
	CHECK(Stat(1) == -15);

	// a client that closed the session does not hear of the server's death (DSession::Detach on an IsClosing session)
	Setup();
	const uint32_t sw = NewServer(2, "protsrv.cl", 1), sx = NewSession(1, "protsrv.cl");
	Send(1, sx, -1, S(0));
	Receive(2, sw, S(1), AMsg);
	Call(2, "EExecSetSessionPtr", R(AMsg), 1);
	Call(2, "EExecMessageComplete", R(AMsg), 0);
	Send(1, sx, 4, S(2));
	Call(1, "EExecHandleClose", sx);					// disconnect queued
	const int rq = Reqs(1);
	Call(2, "EExecThreadKill", KHandleThread0 + 2, 0, 0, 0);
	CHECK(Stat(2) == KPending && Reqs(1) == rq);
	CHECK(!gGSess[sx - KGHandleSess0].iUsed);

	// an unnamed server belongs to the process: its thread's death does not close it
	Setup();
	const uint32_t an = NewServer(2, "", 1);
	Call(2, "EExecThreadKill", KHandleThread0 + 2, 0, 0, 0);
	CHECK(gGSrv[an - KGHandleSrv0].iUsed);
}

static void TestDisconnect()
{
	printf("session disconnect (DSession::Close / CloseFromDisconnect, sipc.cpp:1175, 1197)\n");
	Setup();
	const uint32_t srv = NewServer(2, "protsrv.disc", 1), ses = NewSession(1, "protsrv.disc");
	Send(1, ses, -1, S(0));
	Receive(2, srv, S(1), AMsg);
	Call(2, "EExecSetSessionPtr", R(AMsg), 1);
	Call(2, "EExecMessageComplete", R(AMsg), 0);
	Send(1, ses, 4, S(2));						// a request still queued when the client closes the session
	Call(1, "EExecHandleClose", ses);
	Receive(2, srv, S(3), AMsg);					// 4 comes first, then the disconnect message
	CHECK(R(AMsg + 4) == 4);
	Receive(2, srv, S(4), AMsg + 64);
	CHECK((int32_t)R(AMsg + 68) == -2);
	const int rq = Reqs(1);
	Call(2, "EExecMessageComplete", R(AMsg + 64), 0);		// completing the disconnect message frees the session
	CHECK(!gGSess[ses - KGHandleSess0].iUsed);
	CHECK(Stat(2) == KPending && Reqs(1) == rq);			// the closed session's request never completes
	CHECK(!Exec(2, "EExecMessageComplete", R(AMsg), 0));		// the message handle is stale now: not one of ours
	int used = 0;
	for (int i = 0; i < KGMaxMsg; ++i) used += gGMsg[i].iUsed;
	CHECK(used == 0);							// message 4 (accepted, never completed) went back to the pool with the session
}

int main()
{
	TestBasicAndComplete();
	TestCompleteWithHandle();
	TestOpenObject();
	TestNativeHandlesHalt();
	TestKill();
	TestTransfer();
	TestServerDeath();
	TestDisconnect();
	printf("guest_servers: %d checks, %d failed -> %s\n", gChecks, gFail, gFail ? "FAIL" : "PASS");
	return gFail ? 1 : 0;
}

// proteusgl.cpp (linked for proteusrt.cpp's GL hooks) needs the window-surface hook; a host test has no window
namespace proteus { bool GlWindowSurface(CArmCpu&, THle&, uint32_t) { return false; } }

// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusleave.cpp -- rung R1 leave gate: ARM TRAP / Leave / cleanup stack over the NATIVE euser through the bridge. Built in the device model
// (TRAP = MarkCleanupStack + try/catch, the throw and the unwinder in the guest's drtaeabi.dll). Covers: a plain leave, cleanup items destroyed
// by the native cleanup stack, nested TRAPs, LeaveIfError, an ARM RunL that leaves into the NATIVE scheduler's TRAP (RunError), and a TRAP on a
// guest thread (a native thread). Exit code = the number of the first failing check, 0 = all pass.
#include <e32base.h>

#define CHECK(n, c) do { if (!(c)) return (n); } while (0)

static TInt gDestroyed;
static void Destroy(TAny* aPtr) { gDestroyed += (TInt)aPtr; }
static void LeaveWith(TInt aErr) { User::Leave(aErr); }

static void PushAndLeaveL()
	{
	CleanupStack::PushL(TCleanupItem(Destroy, (TAny*)10));
	CleanupStack::PushL(TCleanupItem(Destroy, (TAny*)20));
	LeaveWith(-7);
	CleanupStack::PopAndDestroy(2);
	}

static TInt NestedL()
	{
	TRAPD(inner, LeaveWith(-3));
	return inner;
	}

static TInt Traps()
	{
	TRAPD(r1, LeaveWith(-5));
	CHECK(1, r1 == -5);
	gDestroyed = 0;
	TRAPD(r2, PushAndLeaveL());
	CHECK(2, r2 == -7);
	CHECK(3, gDestroyed == 30);
	TInt inner = 0;
	TRAPD(r3, inner = NestedL());
	CHECK(4, r3 == 0 && inner == -3);
	TRAPD(r4, User::LeaveIfError(-9));
	CHECK(5, r4 == -9);
	TRAPD(r5, User::LeaveIfError(3));
	CHECK(6, r5 == 0);
	// a NATIVE ...L export that leaves (the native AllocL): the bridge runs it under a native TRAP and raises the leave in the guest
	TRAPD(r6, User::AllocL(0x3FFFFFF0));
	CHECK(7, r6 == KErrNoMemory);
	return 0;
	}

// an ARM RunL that leaves: the NATIVE CActiveScheduler's TRAP catches it and calls the ARM RunError with the reason
class CLeaver : public CActive
	{
public:
	CLeaver() : CActive(EPriorityStandard) { CActiveScheduler::Add(this); }
	~CLeaver() { Cancel(); }
	void Go() { iStatus = KRequestPending; SetActive(); TRequestStatus* p = &iStatus; User::RequestComplete(p, KErrNone); }
	TInt iError;
private:
	void RunL() { User::Leave(-11); }
	TInt RunError(TInt aError) { iError = aError; CActiveScheduler::Stop(); return KErrNone; }
	void DoCancel() {}
	};

static TInt ActiveLeave()
	{
	CActiveScheduler* s = new CActiveScheduler;
	CHECK(10, s != NULL);
	CActiveScheduler::Install(s);
	CLeaver* l = new CLeaver;
	l->Go();
	CActiveScheduler::Start();
	CHECK(11, l->iError == -11);
	delete l;
	CActiveScheduler::Install(NULL);
	delete s;
	return 0;
	}

static TInt ThreadFn(TAny*)
	{
	CTrapCleanup* tc = CTrapCleanup::New();
	TRAPD(r, LeaveWith(-2));
	TInt in = 0;
	TRAPD(r2, in = NestedL());
	delete tc;
	return (r == -2 && r2 == 0 && in == -3) ? 77 : 1;
	}

static TInt ThreadLeave()
	{
	RThread t;
	CHECK(20, t.Create(_L("proteusleave-worker"), ThreadFn, 0x4000, NULL, NULL) == KErrNone);
	TRequestStatus st;
	t.Logon(st);
	t.Resume();
	User::WaitForRequest(st);
	CHECK(21, st.Int() == 77);
	t.Close();
	return 0;
	}

// dynamic_cast over euser classes: the guest's drtaeabi walks the type_info chain the bridge synthesized for the native (-fno-rtti) euser
class CCastee : public CActive
	{
public:
	CCastee() : CActive(EPriorityStandard) {}
private:
	void RunL() {}
	void DoCancel() {}
	};

static TInt Casts()
	{
	CBase* b = new CCastee;
	CHECK(30, b != NULL);
	CHECK(31, dynamic_cast<CActive*>(b) == static_cast<CActive*>(static_cast<CCastee*>(b)));
	CHECK(32, dynamic_cast<CTimer*>(b) == NULL);
	CHECK(33, dynamic_cast<CCastee*>(static_cast<CActive*>(static_cast<CCastee*>(b))) != NULL);
	delete b;
	return 0;
	}

GLDEF_C TInt E32Main()
	{
	TInt r = Traps();
	if (!r) r = ActiveLeave();
	if (!r) r = ThreadLeave();
	if (!r) r = Casts();
	return r;
	}

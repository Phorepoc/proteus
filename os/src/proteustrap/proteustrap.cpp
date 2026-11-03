// proteustrap.cpp -- OURS (Proteus P2, docs/phorepoc/proteus-p2.md): TRAP/Leave and the cleanup stack in guest ARM code.
// Each step prints a line; the last line is "PH: proteus trap test ok" only if every result matched.
#include <e32base.h>
#include <e32debug.h>

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
	CleanupStack::PushL(TCleanupItem(Destroy, (TAny*)100));
	CleanupStack::PopAndDestroy();
	return inner;
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();
	TBool ok = tc != NULL;
	TRAPD(r1, LeaveWith(-1));
	RDebug::Printf("PH: proteus trap 1: leave -1 -> %d", r1);
	ok = ok && r1 == -1;
	gDestroyed = 0;
	TRAPD(r2, PushAndLeaveL());
	RDebug::Printf("PH: proteus trap 2: leave with 2 items -> %d, destroyed %d", r2, gDestroyed);
	ok = ok && r2 == -7 && gDestroyed == 30;
	gDestroyed = 0;
	TInt inner = 0;
	TRAPD(r3, inner = NestedL());
	RDebug::Printf("PH: proteus trap 3: nested -> %d inner %d destroyed %d", r3, inner, gDestroyed);
	ok = ok && r3 == 0 && inner == -3 && gDestroyed == 100;
	HBufC8* b = NULL;
	TRAPD(r4, b = HBufC8::NewLC(16); CleanupStack::PopAndDestroy(b));
	RDebug::Printf("PH: proteus trap 4: NewLC + PopAndDestroy -> %d", r4);
	ok = ok && r4 == 0;
	RDebug::Printf("PH: proteus trap test %s", ok ? "ok" : "WRONG");
	delete tc;
	return ok ? KErrNone : KErrGeneral;
	}

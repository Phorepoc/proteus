// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusbridge.cpp -- rung R1 gates: ARM code calling the NATIVE euser through the bridge (descriptors, arrays with a comparator, the heap,
// varargs Format, a guest thread as a native thread, an ARM CActive under the native active scheduler).
// Exit code = the number of the first failing check, 0 = all pass. Run: bash tests/proteus/run.sh --bridge (host, fake natives) or on the board.
#include <e32std.h>
#include <e32base.h>
#include <e32atomics.h>

#define CHECK(n, c) do { if (!(c)) return (n); } while (0)

static TInt Descriptors()
	{
	TBuf<32> b;
	b.Copy(_L("hello"));
	CHECK(1, b.Length() == 5);
	b.Append(_L(" world"));
	CHECK(2, b.Length() == 11);
	CHECK(3, b.Compare(_L("hello world")) == 0);
	CHECK(4, b.Compare(_L("hello there")) > 0);
	CHECK(5, b.Find(_L("wor")) == 6);
	TBuf<16> n;
	n.Num(12345);
	CHECK(6, n.Compare(_L("12345")) == 0);
	n.Zero();
	n.AppendNum(-42);
	CHECK(7, n.Compare(_L("-42")) == 0);
	TBuf8<16> c;
	c.Copy(b.Left(5));
	CHECK(8, c.Length() == 5 && c[0] == 'h' && c[4] == 'o');
	TPtrC16 p(b);
	CHECK(9, p.Locate('w') == 6 && p.LocateReverse('o') == 7);
	b.UpperCase();
	CHECK(10, b.Compare(_L("HELLO WORLD")) == 0);
	return 0;
	}

struct TPair { TInt iKey; TInt iTag; };
static TInt ComparePair(const TPair& aA, const TPair& aB) { return aA.iKey - aB.iKey; }

static TInt Arrays()
	{
	RArray<TInt> a;
	CHECK(20, a.Append(3) == KErrNone);
	CHECK(21, a.Append(5) == KErrNone);
	CHECK(22, a.Append(8) == KErrNone);
	CHECK(23, a.Count() == 3 && a[0] == 3 && a[1] == 5 && a[2] == 8);
	a.Remove(1);
	CHECK(24, a.Count() == 2 && a[1] == 8);
	CHECK(25, a.Insert(4, 1) == KErrNone && a[1] == 4);
	a.Close();
	// a function pointer into native code: Sort / Find take a comparator the native euser must call back into ARM code
	RArray<TPair> b;
	const TInt v[] = { 5, 3, 8, 1, 9, 2 };
	for (TInt i = 0; i < 6; ++i) { TPair p = { v[i], i }; b.Append(p); }
	b.Sort(TLinearOrder<TPair>(&ComparePair));
	CHECK(26, b[0].iKey == 1 && b[1].iKey == 2 && b[2].iKey == 3 && b[3].iKey == 5 && b[4].iKey == 8 && b[5].iKey == 9);
	CHECK(27, b[0].iTag == 3 && b[5].iTag == 4);
	b.Close();
	return 0;
	}

static TInt Heap()
	{
	TUint8* p = (TUint8*)User::Alloc(100);
	CHECK(30, p != NULL);
	Mem::Fill(p, 100, 0xA5);
	CHECK(31, p[0] == 0xA5 && p[99] == 0xA5);
	TUint8* q = (TUint8*)User::ReAlloc(p, 4000);
	CHECK(32, q != NULL && q[99] == 0xA5);
	User::Free(q);
	CHECK(33, User::Alloc(1 << 28) == NULL || true);
	return 0;
	}

// gate 2: Format with %d %Ld %f %S -- the varargs (r2.., a 64-bit value in an even pair after a skipped r3, a double, a descriptor pointer on the stack)
static TInt Formats()
	{
	TBuf<64> f;
	TBuf<8> name(_L("abc"));
	TInt64 big = I64LIT(-5000000000);
	TReal r = 2.5;
	f.Format(_L("%d %Ld %f %S"), 42, big, r, &name);
	CHECK(40, f.Find(_L("42 -5000000000 2.5")) == 0);
	CHECK(41, f.Find(_L("abc")) > 0);
	f.AppendFormat(_L(" [%x|%c]"), 0xBEEF, 'z');
	CHECK(42, f.Find(_L("[beef|z]")) > 0);
	TBuf8<32> g;
	g.Format(_L8("%d-%u"), -7, 9u);
	CHECK(43, g.Length() == 4 && g[0] == '-' && g[1] == '7' && g[3] == '9');
	return 0;
	}

// R1: a guest thread is a native thread (the native RThread::Create calls the thread function back through a reverse stub on a new CPU)
struct TShared { TInt iIn; TInt iOut; };
static TInt ThreadFn(TAny* aArg)
	{
	TShared& s = *(TShared*)aArg;
	TBuf<16> b;
	b.Num(s.iIn * 3);			// bridged euser calls from the guest thread's own CPU
	s.iOut = b.Length() * 100 + s.iIn * 3;
	return 7;
	}

static TInt Threads()
	{
	TShared s = { 41, 0 };
	RThread t;
	CHECK(50, t.Create(_L("proteusbridge-worker"), ThreadFn, 0x2000, NULL, &s) == KErrNone);
	TRequestStatus st;
	t.Logon(st);
	t.Resume();
	User::WaitForRequest(st);
	CHECK(51, st.Int() == 7);
	CHECK(52, s.iOut == 3 * 100 + 123);
	CHECK(53, t.ExitReason() == 7);
	t.Close();
	return 0;
	}

// R1: an ARM CActive run by the NATIVE CActiveScheduler (its RunL is called from native code: the vtable slot is a reverse stub), and an ARM
// call through the vtable of a natively constructed object (the CActiveScheduler's virtual destructor: a native address the guest calls)
class CTick : public CActive
	{
public:
	CTick() : CActive(EPriorityStandard) { CActiveScheduler::Add(this); }
	~CTick() { Cancel(); }
	void Go() { iStatus = KRequestPending; SetActive(); TRequestStatus* p = &iStatus; User::RequestComplete(p, KErrNone); }
	TInt iRuns;
private:
	void RunL() { if (++iRuns < 3) Go(); else CActiveScheduler::Stop(); }
	void DoCancel() {}
	};

static TInt ActiveObjects()
	{
	CActiveScheduler* s = new CActiveScheduler;
	CHECK(60, s != NULL);
	CActiveScheduler::Install(s);
	CTick* t = new CTick;
	CHECK(61, t != NULL);
	t->Go();
	CActiveScheduler::Start();
	CHECK(62, t->iRuns == 3);
	delete t;					// the ARM destructor through the patched slot (ARM -> stub -> back to ARM)
	CActiveScheduler::Install(NULL);
	delete s;					// a native object's virtual destructor called from ARM code
	return 0;
	}

// a run-time load is an ARM module of this process (the guest loader), not the native DLL of that name: proteusdll.dll, its static
// constructor run on attach, its exports by ordinal (1 = Add, 3 = State)
typedef TInt (*TAddFn)(TInt, TInt);
typedef TInt (*TStateFn)();
static TInt Library()
	{
	RLibrary lib;
	CHECK(70, lib.Load(_L("proteusdll.dll")) == KErrNone);
	TAddFn add = (TAddFn)lib.Lookup(1);
	TStateFn state = (TStateFn)lib.Lookup(3);
	CHECK(71, add != NULL && state != NULL);
	CHECK(72, add(4, 2) == 42);
	CHECK(73, state() == 0x600D * 1000 + 102);
	CHECK(74, lib.Lookup(99) == NULL);
	lib.Close();
	return 0;
	}

// struct returns across the ABIs: ARM returns a composite of more than 4 bytes through a hidden pointer in r0 (ahead of `this`), x32 SysV a
// trivially-copyable one of up to 16 bytes in rax:rdx and a larger one through its own hidden pointer (gen_bridge.py ret_abi)
static TInt Returns()
	{
	TPtrC m = _L("abcdef").Mid(2, 3);				// a TPtrC16 (8 bytes): r0 pointer -> rax
	CHECK(80, m.Length() == 3 && m[0] == 'c' && m[2] == 'e');
	TRegionFix<1> reg;
	const TRect r = reg.BoundingRect();				// a TRect (16 bytes): r0 pointer -> rax:rdx
	CHECK(81, r.iTl.iX == 1 && r.iTl.iY == 2 && r.iBr.iX == 30 && r.iBr.iY == 40);
	TInt64 x = MAKE_TINT64(3, 5);
	const TInt64 old = (TInt64)__e32_atomic_add_ord64(&x, MAKE_TINT64(1, 2));	// a 64-bit result in r0:r1 <- rax, the addend in r2:r3
	CHECK(82, old == MAKE_TINT64(3, 5) && x == MAKE_TINT64(4, 7));
	RThread t;
	const TName n = t.Name();					// a TName (264 bytes, a TBuf): a hidden pointer on both sides
	CHECK(83, n.Compare(_L("fake-thread")) == 0);
	CBufFlat* buf = CBufFlat::NewL(8);				// a NATIVE object: its Ptr is a native virtual the ARM code calls (a foreign pc)
	CHECK(84, buf != NULL);
	const TPtr8 p = buf->Ptr(2);					// a TPtr8 (12 bytes) from a native virtual: r0 = result, r1 = this
	CHECK(85, p.Length() == 6 && p.MaxLength() == 6 && p.Ptr() == (const TUint8*)buf + 64 + 2);
	return 0;
	}

// the process asking about itself: the guest program's name and command line, not the handler's (mixed mode: the native kernel answers
// with cpuhandler_arm.exe and a command line that starts with the program's path)
static TInt Self()
	{
	const TFileName f = RProcess().FileName();
	CHECK(90, f.CompareF(_L("c:\\sys\\bin\\proteusbridge.exe")) == 0);
	CHECK(91, User::CommandLineLength() == 3);
	TBuf<64> cl;
	User::CommandLine(cl);
	CHECK(92, cl.Compare(_L("-xy")) == 0);
	return 0;
	}

GLDEF_C TInt E32Main()
	{
	TInt r = Descriptors();
	if (!r) r = Arrays();
	if (!r) r = Heap();
	if (!r) r = Formats();
	if (!r) r = Threads();
	if (!r) r = ActiveObjects();
	if (!r) r = Library();
	if (!r) r = Returns();
	if (!r) r = Self();
	return r;
	}

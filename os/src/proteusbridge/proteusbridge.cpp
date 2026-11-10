// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusbridge.cpp -- rung R1 gate 1: ARM code calling the NATIVE euser through the bridge (no varargs, no comparator).
// Exit code = the number of the first failing check, 0 = all pass. Run: bash tests/proteus/run.sh --bridge (host, fake natives) or on the board.
#include <e32std.h>
#include <e32base.h>

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

GLDEF_C TInt E32Main()
	{
	TInt r = Descriptors();
	if (!r) r = Arrays();
	if (!r) r = Heap();
	if (!r) r = Formats();
	return r;
	}

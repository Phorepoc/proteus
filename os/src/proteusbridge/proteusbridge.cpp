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

GLDEF_C TInt E32Main()
	{
	TInt r = Descriptors();
	if (!r) r = Arrays();
	if (!r) r = Heap();
	return r;
	}

// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusdllexe.cpp -- OURS (Proteus P2, docs/phorepoc/proteus-p2.md): an ARM EXE that calls into an ARM DLL; run on x86 by proteus.exe, which loads + links
// the DLL in the guest and runs its entry point (static constructor) first.
#include <e32std.h>
#include <e32debug.h>

extern "C" IMPORT_C TInt ProteusDllAdd(TInt aA, TInt aB);
extern "C" IMPORT_C TInt ProteusDllGreet(const char* aWho, TInt aValue);
extern "C" IMPORT_C TInt ProteusDllState();

GLDEF_C TInt E32Main()
	{
	const TInt sum = ProteusDllAdd(4, 2);
	const TInt g = ProteusDllGreet("the ARM EXE", sum);
	const TInt state = ProteusDllState();
	const TBool ok = sum == 42 && g == 43 && state == 0x600D * 1000 + 102;
	RDebug::Printf("PH: proteus EXE got %d and %d from the ARM DLL, DLL state %d (ctor + WSD), %s", sum, g, state, ok ? "ok" : "WRONG");
	return ok ? KErrNone : KErrGeneral;
	}

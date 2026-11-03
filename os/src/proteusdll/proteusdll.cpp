// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusdll.cpp -- OURS (Proteus P2, docs/phorepoc/proteus-p2.md): a real ARM DLL that proteus.exe links into the guest for proteusdllexe.exe.
// Ordinal 1 is computation + writable static data (gCalls, .data); ordinal 2 calls euser itself (RDebug::Printf); ordinal 3 reports the state a
// static constructor set (.bss, run by _E32Dll on process attach) -- so imports, WSD and the DLL entry point are all exercised.
#include <e32std.h>
#include <e32debug.h>

static TInt gCalls = 100;				// .data: initialised writable static data (EPOCALLOWDLLDATA)
static TInt gCtorMagic;					// .bss: set by the static constructor below

class TInitOnLoad
	{
public:
	TInitOnLoad() { gCtorMagic = 0x600D; ++gCalls; }	// gCalls 101 after the constructor ran
	};
static TInitOnLoad gInit;

extern "C" EXPORT_C TInt ProteusDllAdd(TInt aA, TInt aB)
	{
	++gCalls;
	return aA * 10 + aB;
	}

extern "C" EXPORT_C TInt ProteusDllGreet(const char* aWho, TInt aValue)
	{
	RDebug::Printf("PH: proteus ARM DLL says hello to %s, value %d", aWho, aValue);
	return aValue + 1;
	}

extern "C" EXPORT_C TInt ProteusDllState()
	{
	return gCtorMagic * 1000 + gCalls;		// expected after one Add: 0x600D * 1000 + 102
	}

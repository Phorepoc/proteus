// proteushello.cpp -- OURS (Proteus P0, docs/phorepoc/research/proteus-x86.md): the smallest REAL ARM Symbian EXE. Built for ARM (./ph arm proteushello, E32 CPU id ARMv5),
// run on the x86 board by proteus.exe (the translator/interpreter), it prints one line to the kernel log. Imports euser only.
#include <e32std.h>
#include <e32debug.h>

GLDEF_C TInt E32Main()
	{
	RDebug::Printf("PH: proteus hello from ARM code: %d + %d = %d, %s", 40, 2, 40 + 2, "ok");
	return KErrNone;
	}

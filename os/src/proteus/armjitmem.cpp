// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armjitmem.cpp -- OURS (Proteus P1): the translator's executable memory (patches/dynarmic/0005, DYNARMIC_EXTERNAL_CODE_MEMORY).
// Each allocation is a local code chunk (RChunk::CreateLocalCode: user read/write/execute, the kernel's memory for JITs), reserved at
// its full size and committed from the bottom as dynarmic fills it (dynarmic_code_commit, the counterpart of VirtualAlloc(MEM_COMMIT)).
// dynarmic makes two: the code cache (TArmJit's code_cache_size) and the spin lock's 4 KB.
#ifdef PROTEUS_JIT
#include <e32std.h>
#include <e32debug.h>
#include <stddef.h>

namespace
{
struct TCodeChunk
	{
	TUint8* iBase;
	TInt iSize;
	RChunk iChunk;
	};
const TInt KMaxCodeChunks = 64;		// one per guest CPU (the code cache) + the spin lock's: allocated under RunOnNewCpu's JIT lock
TCodeChunk gCode[KMaxCodeChunks];

TCodeChunk* Find(const void* aP)
	{
	for (TInt i = 0; i < KMaxCodeChunks; ++i)
		if (gCode[i].iBase && (const TUint8*)aP >= gCode[i].iBase && (const TUint8*)aP < gCode[i].iBase + gCode[i].iSize)
			return &gCode[i];
	return NULL;
	}
}

extern "C" void* dynarmic_code_alloc(size_t aSize)
	{
	const TInt size = (TInt)((aSize + 0xFFF) & ~(size_t)0xFFF);
	for (TInt i = 0; i < KMaxCodeChunks; ++i)
		{
		if (gCode[i].iBase)
			continue;
		RChunk c;
		const TInt r = c.CreateLocalCode(0, size);
		if (r != KErrNone)
			{
			RDebug::Printf("PH: proteus jit: code chunk of %d KB: %d", size >> 10, r);
			return NULL;
			}
		gCode[i].iChunk = c;
		gCode[i].iBase = c.Base();
		gCode[i].iSize = size;
		return c.Base();
		}
	return NULL;
	}

extern "C" void dynarmic_code_commit(void* aP, size_t aSize)
	{
	TCodeChunk* c = Find(aP);
	if (!c)
		return;
	const TInt want = (TInt)(((size_t)((TUint8*)aP - c->iBase) + aSize + 0xFFF) & ~(size_t)0xFFF);
	if (want > c->iChunk.Size())
		{
		const TInt r = c->iChunk.Adjust(want < c->iSize ? want : c->iSize);
		if (r != KErrNone)
			RDebug::Printf("PH: proteus jit: code chunk commit %d KB: %d", want >> 10, r);	// dynarmic then writes past the commit: a fault, not silence
		}
	}

extern "C" void dynarmic_code_free(void* aP)
	{
	TCodeChunk* c = Find(aP);
	if (!c)
		return;
	c->iChunk.Close();
	c->iBase = NULL;
	c->iSize = 0;
	}
#endif

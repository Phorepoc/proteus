// proteusheap.cpp -- OURS (Proteus P2, docs/phorepoc/proteus-p2.md): RHeap stress on the real ARM euser -- every live block is filled
// with its own tag; any overlap (the heap handing out memory twice) or a lost tag is reported. Last line "PH: proteus heap test ok".
#include <e32std.h>
#include <e32debug.h>

const TInt KSlots = 64;
static TUint8* gP[KSlots];
static TInt gLen[KSlots];

static TBool Check(TInt aSlot)
	{
	for (TInt i = 0; i < gLen[aSlot]; ++i)
		if (gP[aSlot][i] != (TUint8)(aSlot + 1))
			{
			RDebug::Printf("PH: proteus heap: slot %d (%x, %d bytes) overwritten at +%d: %x", aSlot, (TUint)gP[aSlot], gLen[aSlot], i, gP[aSlot][i]);
			return EFalse;
			}
	return ETrue;
	}

GLDEF_C TInt E32Main()
	{
	TUint32 seed = 12345;
	TBool ok = ETrue;
	for (TInt round = 0; round < 20000 && ok; ++round)
		{
		seed = seed * 1103515245 + 12345;
		const TInt s = (seed >> 8) % KSlots;
		const TInt len = ((seed >> 20) & 1) ? 1 + ((seed >> 16) % 96) : 1 + ((seed >> 16) % 5000);	// many small (slab-sized) cells + some big ones
		if (gP[s] && !(ok = Check(s))) break;
		switch ((seed >> 4) & 3)
			{
			case 0: User::Free(gP[s]); gP[s] = NULL; gLen[s] = 0; break;
			case 1:
				{
				TUint8* p = (TUint8*)User::ReAlloc(gP[s], len);
				if (p) { gP[s] = p; if (len > gLen[s]) Mem::Fill(p + gLen[s], len - gLen[s], s + 1); gLen[s] = len; }
				break;
				}
			default:
				User::Free(gP[s]);
				gP[s] = (TUint8*)User::Alloc(len);
				gLen[s] = gP[s] ? len : 0;
				if (gP[s]) Mem::Fill(gP[s], len, s + 1);
				break;
			}
		if ((round & 63) == 0)
			for (TInt k = 0; k < KSlots && ok; ++k) if (gP[k]) ok = Check(k);
		}
	RDebug::Printf("PH: proteus heap test %s", ok ? "ok" : "WRONG");
	return ok ? KErrNone : KErrGeneral;
	}

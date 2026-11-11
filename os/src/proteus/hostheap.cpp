// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// hostheap.cpp -- OURS (Proteus): the handler's own memory stays on the handler's heap.
// With a native euser the guest calls User::SwitchAllocator natively, so the thread's current allocator becomes the guest's heap
// (PyS60: a 1 MB UserHeap::ChunkHeap). Everything Proteus and the translator allocate through operator new or User::Alloc would
// then land in that heap, and a block allocated before the switch would be freed into it afterwards -- the free list is corrupt
// from then on. The process' first heap is kept here (HostHeapInit, at E32Main): the bridge (TBridgeThreads::iHostAlloc) puts the
// thread back on it whenever Proteus runs, and guest stacks / bridge buffers come from it directly (HostAlloc, HostFree).
#include <e32std.h>

static RHeap* gHostHeap;
static TUint8* gHostLo;
static TUint8* gHostHi;

void HostHeapInit()
	{
	if (gHostHeap) return;
	gHostHeap = &User::Heap();
	gHostLo = gHostHeap->Base();
	gHostHi = gHostLo + gHostHeap->MaxLength();
	}

TAny* HostAlloc(TInt aSize)
	{
	return gHostHeap ? gHostHeap->Alloc(aSize) : User::Alloc(aSize);
	}

void HostFree(TAny* aPtr)
	{
	if (!aPtr) return;
	if (gHostHeap && (TUint8*)aPtr >= gHostLo && (TUint8*)aPtr < gHostHi) gHostHeap->Free(aPtr);
	else User::Free(aPtr);
	}

TUint32 HostAllocator() { return (TUint32)gHostHeap; }
TUint32 HostSwitchAllocator(TUint32 aAlloc) { return (TUint32)User::SwitchAllocator((RAllocator*)aAlloc); }

// zeroed read/write/execute memory (the bridge's reverse stubs): a local code chunk each, kept for the life of the process
TUint32 HostCodeAlloc(TUint32 aBytes)
	{
	const TInt size = (TInt)((aBytes + 0xFFF) & ~0xFFFu);
	RChunk c;
	if (c.CreateLocalCode(size, size) != KErrNone) return 0;
	Mem::FillZ(c.Base(), size);
	return (TUint32)c.Base();
	}

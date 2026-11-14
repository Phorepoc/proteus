// Copyright The Phorepoc Authors
// SPDX-License-Identifier: AGPL-3.0-only
// hostheap.cpp -- OURS (Proteus): the handler's own memory stays on the handler's heap.
// With a native euser the guest calls User::SwitchAllocator natively, so the thread's current allocator becomes the guest's heap
// (PyS60: a 1 MB UserHeap::ChunkHeap). Everything Proteus and the translator allocate through operator new or User::Alloc would
// then land in that heap, and a block allocated before the switch would be freed into it afterwards -- the free list is corrupt
// from then on. The process' first heap is kept here (HostHeapInit, at E32Main): the bridge (TBridgeThreads::iHostAlloc) puts the
// thread back on it whenever Proteus runs, and guest stacks / bridge buffers come from it directly (HostAlloc, HostFree).
#include <e32std.h>
#include <e32debug.h>

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

// The C++ runtime archives (libgcc_eh, libphcompat) name three weak glibc/engine hooks no provider exports; each would be a link
// stub (a call panics EUSRNIMP; the pdk's package stub audit refuses it), so the handler answers them itself.
// libgcc_eh's unwinder asks glibc's _dl_find_object for a pc's frame tables: "not found" (-1) is the answer for an E32 image
// (x32 unwinding is unfinished, a throw ends the process)
extern "C" int _dl_find_object(void*, void*) { return -1; }
// libgcc's gthr "is libpthread linked" probe (a weak glibc symbol): present, it forwards to Open C's pthread_key_create
extern "C" int pthread_key_create(unsigned*, void (*)(void*));
extern "C" int __pthread_key_create(unsigned* aKey, void (*aDtor)(void*)) { return pthread_key_create(aKey, aDtor); }
// libphcompat's allocator calls this when malloc fails: the log names the request (the process then ends on its own)
extern "C" void ph_oom_report(unsigned aSize)
	{
	RDebug::Printf("PH: [proteus] OOM: malloc(%u) failed", aSize);
	}

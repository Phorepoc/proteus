// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusrt.cpp -- OURS (Proteus P0). See proteusrt.h.
#include "proteusrt.h"
#include "e32decomp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

namespace proteus
{
struct TOrd { unsigned short iOrd; const char* iName; };
static const TOrd KEuser[] =
{
#include "euser_ordinals.inc"
};

const char* EuserName(uint32_t aOrdinal)
{
	int lo = 0, hi = (int)(sizeof(KEuser) / sizeof(KEuser[0])) - 1;
	while (lo <= hi)
	{
		const int mid = (lo + hi) / 2;
		if (KEuser[mid].iOrd == aOrdinal) return KEuser[mid].iName;
		if (KEuser[mid].iOrd < aOrdinal) lo = mid + 1; else hi = mid - 1;
	}
	return "";
}

static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void Err(TLoaded& o, const char* m) { strncpy(o.iError, m, sizeof(o.iError) - 1); }

static bool LoadE32At(CArmCpu& cpu, const uint8_t* f, uint32_t len, uint32_t at, TLoaded& o, TProcess* proc, TFindDllFn find, void* findCtx);

bool LoadE32(CArmCpu& cpu, const uint8_t* f, uint32_t len, uint32_t at, TLoaded& o)
{
	return LoadE32At(cpu, f, len, at, o, NULL, NULL, NULL);
}

// the DLL base name of an import name: "proteusdll{000a0000}[e70157f2].dll" -> "proteusdll.dll"
// "CONE{000a0000}.DLL" -> "cone.dll": lower case, as module and file names are case-insensitive (an app importing CONE.DLL and one
// importing cone.dll share one module -- two copies would each have their own Dll::Tls, so CCoeEnv::Static() is null in one of them)
static void BaseName(const char* in, char* out, int max)
{
	int n = 0;
	const char* ext = strrchr(in, '.');
	for (const char* s = in; *s && *s != '{' && *s != '[' && s != ext && n < max - 5; ++s) out[n++] = *s;
	out[n] = 0;
	strcat(out, ext ? ext : ".dll");
	for (char* s = out; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

void FreeProcess(TProcess& p)
{
	free(p.iSlots);
	p.iSlots = NULL;
	p.iSlotCount = p.iSlotMax = 0;
}

bool LoadProcess(CArmCpu& cpu, const uint8_t* f, uint32_t len, uint32_t at, TFindDllFn find, void* findCtx, TProcess& out)
{
	const bool realEuser = out.iRealEuser;
	memset(&out, 0, sizeof(out));
	out.iRealEuser = realEuser;
	out.iCount = 1;
	const bool ok = LoadE32At(cpu, f, len, at, out.iMod[0], &out, find, findCtx);
	if (!ok) strncpy(out.iError, out.iMod[0].iError, sizeof(out.iError) - 1);
	return ok;
}

static bool LoadE32At(CArmCpu& cpu, const uint8_t* f, uint32_t len, uint32_t at, TLoaded& o, TProcess* proc, TFindDllFn find, void* findCtx)
{
	const int myIndex = proc ? (int)(&o - proc->iMod) : 0;
	char keepName[40];
	strcpy(keepName, o.iName);
	const char keepDrive = o.iDrive;
	memset(&o, 0, sizeof(o));
	strcpy(o.iName, keepName);
	o.iDrive = keepDrive;
	o.iModule = myIndex;
	if (len < 124 || memcmp(f + 16, "EPOC", 4) != 0) { Err(o, "not an E32Image"); return false; }
	if (U32(f + 28) != 0)
	{
		// P2: byte-pair / deflate -> the raw image, then load that (the file offsets of the raw image are the header's)
		uint32_t rawLen = 0;
		const char* why = "";
		uint8_t* raw = DecompressE32(f, len, rawLen, why);
		if (!raw) { snprintf(o.iError, sizeof(o.iError), "compressed E32Image: %s", why); return false; }
		strcpy(o.iName, keepName);
		const bool ok = LoadE32At(cpu, raw, rawLen, at, o, proc, find, findCtx);
		free(raw);
		return ok;
	}
	o.iCpu = f[122] | (f[123] << 8);
	if (o.iCpu < 0x2000 || o.iCpu > 0x2002) { Err(o, "not an ARM image (CPU id)"); return false; }
	o.iUid1 = U32(f + 0);
	o.iProcPriority = f[120] | (f[121] << 8);		// E32ImageHeaderV::iProcessPriority (0x78)
	o.iHeapMin = U32(f + 56);
	o.iHeapMax = U32(f + 60);
	o.iStackSize = U32(f + 64);
	o.iUid3 = U32(f + 8);
	o.iSecureId = U32(f + 128); o.iVendorId = U32(f + 132); o.iCaps[0] = U32(f + 136); o.iCaps[1] = U32(f + 140);	// E32ImageHeaderV iS (SSecurityInfo)
	o.iFlags = U32(f + 44);
	const uint32_t codeSize = U32(f + 48), dataSize = U32(f + 52), bss = U32(f + 68), entry = U32(f + 72), codeBase = U32(f + 76), dataBase = U32(f + 80);
	const uint32_t codeOff = U32(f + 100), dataOff = U32(f + 104), impOff = U32(f + 108), cReloc = U32(f + 112), dReloc = U32(f + 116);
	if (codeOff + codeSize > len || (dataSize && dataOff + dataSize > len)) { Err(o, "truncated image"); return false; }
	const uint32_t dataAddr = (at + codeSize + 15) & ~15u;
	const uint32_t end = dataAddr + dataSize + bss;
	if (!cpu.InRegion(at, end - at)) { Err(o, "image does not fit the guest region"); return false; }
	if (!cpu.Commit(at, end - at)) { Err(o, "no memory for the image"); return false; }
	memcpy(cpu.iHost + (at - cpu.iBase), f + codeOff, codeSize);
	if (dataSize) memcpy(cpu.iHost + (dataAddr - cpu.iBase), f + dataOff, dataSize);
	if (bss) memset(cpu.iHost + (dataAddr + dataSize - cpu.iBase), 0, bss);
	o.iLoadAddr = at;
	o.iDelta = at - codeBase;
	const uint32_t dDelta = dataAddr - dataBase;
	o.iEntry = at + entry;			// iEntryPoint is the offset of the entry within the code
	// E32ImageHeader::ExceptionDescriptor(): a code offset with bit 0 set, else none; the kernel adds the code run address (pcodeseg.cpp)
	const uint32_t xd = len >= 148 ? U32(f + 144) : 0;
	o.iExceptionDescriptor = (xd & 1) && xd != 0xFFFFFFFFu ? at + (xd & ~1u) : 0;
	o.iEnd = (end + 0xFFF) & ~0xFFFu;
	// which export directory entries (in the code section) the code relocations move: RVCT's elf2e32 lists them all, our emitter none
	const uint32_t expCount = U32(f + 92), expOff = U32(f + 88) - codeOff;
	const bool expInCode = expCount && U32(f + 88) >= codeOff && U32(f + 88) + 4 * expCount <= codeOff + codeSize;
	struct TBits { uint8_t* p; ~TBits() { free(p); } } expRel = { expInCode ? (uint8_t*)calloc(expCount, 1) : NULL };
	if (expInCode && !expRel.p) { Err(o, "no memory"); return false; }
	// relocations: [size][count] then per-4K-page blocks [base][blocksize][u16 info...]; nibble 0x1000 = text delta, 0x2000 = data delta, 0x3000 = inferred by value
	for (int pass = 0; pass < 2; ++pass)
	{
		const uint32_t ro = pass == 0 ? cReloc : dReloc;
		if (!ro) continue;
		if (ro + 8 > len) { Err(o, "bad relocation section"); return false; }
		const uint32_t total = U32(f + ro + 4);
		uint32_t p = ro + 8, seen = 0;
		while (seen < total)
		{
			if (p + 8 > len) { Err(o, "bad relocation block"); return false; }
			const uint32_t base = U32(f + p), bsize = U32(f + p + 4);
			if (bsize < 8) { Err(o, "bad relocation block size"); return false; }
			for (uint32_t k = 0; k < (bsize - 8) / 2; ++k)
			{
				const uint32_t info = f[p + 8 + 2 * k] | (f[p + 9 + 2 * k] << 8);
				if (!info) continue;
				++seen;
				const uint32_t off = base + (info & 0xFFF), type = info & 0xF000;
				const uint32_t where = (pass == 0 ? at : dataAddr) + off;
				if (pass == 0 && expRel.p && off - expOff < 4 * expCount && !((off - expOff) & 3)) expRel.p[(off - expOff) >> 2] = 1;
				if (!cpu.Valid(where, 4)) { Err(o, "relocation outside the image"); return false; }
				uint32_t w = cpu.Read32(where);
				uint32_t d = type == 0x1000 ? o.iDelta : type == 0x2000 ? dDelta : (w >= dataBase && w < dataBase + dataSize + bss ? dDelta : o.iDelta);
				cpu.Write32(where, w + d);
			}
			p += bsize;
		}
	}
	// export directory: inside the code section; entries are code addresses (codeBase based), relocated by the loader, not by the reloc table
	o.iExportCount = U32(f + 92);
	// Two layouts: elf2e32 / our device images keep it in the code section (link-time addresses; an entry the code relocations already moved
	// -- RVCT's ELF-format images, sf_lepoc.cpp RelocateExports -- stays, the others get the delta); EKA2L1-era images of our emitter append a
	// trailing table (link-time addresses) after the sections -- copied to a page after the image here.
	if (o.iExportCount && U32(f + 88))
	{
		const uint32_t eo = U32(f + 88);
		if (eo + 4 * o.iExportCount > len) { Err(o, "export directory outside the file"); return false; }
		const bool inCode = expInCode;
		o.iExportDir = inCode ? at + (eo - codeOff) : o.iEnd;
		if (!inCode)
		{
			if (!cpu.Commit(o.iEnd, 4 * o.iExportCount)) { Err(o, "no memory for the export directory"); return false; }
			o.iEnd = (o.iEnd + 4 * o.iExportCount + 0xFFF) & ~0xFFFu;
		}
		for (uint32_t i = 0; i < o.iExportCount; ++i)
		{
			const uint32_t a = o.iExportDir + 4 * i;
			if (!cpu.Valid(a, 4)) { Err(o, "export directory outside the guest region"); return false; }
			if (!inCode) cpu.Write32(a, U32(f + eo + 4 * i) + o.iDelta);
			else if (!expRel.p[i]) cpu.Write32(a, cpu.Read32(a) + o.iDelta);
		}
	}
	// imports: [size] then per DLL [name offset][count][count x code offset]; every import word (an ordinal, adj in the high half) becomes a TRAP address
	if (impOff)
	{
		uint32_t p = impOff + 4;
		const uint32_t refs = U32(f + 84);
		if (refs > KMaxDllRefs) { Err(o, "too many imported DLLs"); return false; }
		for (uint32_t d = 0; d < refs; ++d)
		{
			const uint32_t nameOff = U32(f + p), n = U32(f + p + 4);
			p += 8;
			strncpy(o.iDll[d], (const char*)f + impOff + nameOff, sizeof(o.iDll[d]) - 1);
			// P2: a real ARM DLL for this import? (euser stays HLE until the real ARM euser runs)
			o.iDllModule[d] = -1;
			if (proc && find && (proc->iRealEuser || strncmp(o.iDll[d], "euser", 5) != 0))
			{
				char base[40];
				BaseName(o.iDll[d], base, sizeof(base));
				for (int m = 1; m < proc->iCount; ++m)			// already loaded (shared by two importers)?
					if (!strcmp(proc->iMod[m].iName, base)) o.iDllModule[d] = m;
				uint32_t dlen = 0;
				char drive = 0;
				const uint8_t* dimg = o.iDllModule[d] < 0 ? find(base, dlen, drive, findCtx) : NULL;
				if (dimg)
				{
					if (proc->iCount >= KMaxModules) { Err(o, "too many DLLs"); return false; }
					if (dlen < 4 || U32(dimg) != 0x10000079) { snprintf(o.iError, sizeof(o.iError), "%s is not a DLL", base); return false; }
					const int m = proc->iCount++;
					uint32_t next = o.iEnd ? o.iEnd : ((at + U32(f + 48) + U32(f + 52) + U32(f + 68) + 0x1FFF) & ~0xFFFu);
					for (int k = 0; k < proc->iCount - 1; ++k) if (proc->iMod[k].iEnd > next) next = proc->iMod[k].iEnd;
					strncpy(proc->iMod[m].iName, base, sizeof(proc->iMod[m].iName) - 1);
					proc->iMod[m].iDrive = drive;
					if (!LoadE32At(cpu, dimg, dlen, next, proc->iMod[m], proc, find, findCtx))
					{
						snprintf(o.iError, sizeof(o.iError), "%s: %s", base, proc->iMod[m].iError);
						return false;
					}
					o.iDllModule[d] = m;
				}
			}
			for (uint32_t i = 0; i < n; ++i, p += 4)
			{
				const uint32_t off = U32(f + p);
				const uint32_t where = at + off - 0;			// import offsets are code offsets (from the start of the code)
				const uint32_t w = cpu.Read32(where);
				const uint32_t ord = w & 0xFFFF, adj = w >> 16;
				if (o.iDllModule[d] >= 0)
				{
					const TLoaded& dll = proc->iMod[o.iDllModule[d]];
					if (ord < 1 || ord > dll.iExportCount) { snprintf(o.iError, sizeof(o.iError), "ordinal %u not exported by %s", ord, o.iDll[d]); return false; }
					cpu.Write32(where, cpu.Read32(dll.iExportDir + 4 * (ord - 1)) + adj);
				}
				else
				{
					uint32_t slot = 0;
					if (proc)
					{
						if (proc->iSlotCount == proc->iSlotMax)
						{
							const uint32_t nmax = proc->iSlotMax ? proc->iSlotMax * 2 : 1024;
							uint32_t* ns = (uint32_t*)realloc(proc->iSlots, nmax * sizeof(uint32_t));
							if (!ns) { Err(o, "no memory for import slots"); return false; }
							proc->iSlots = ns;
							proc->iSlotMax = nmax;
						}
						slot = proc->iSlotCount++;
						proc->iSlots[slot] = ((uint32_t)myIndex << 22) | (d << 16) | ord;
					}
					else
						slot = (d << 16) | (w & 0x7FFF);		// (single image: DLL ref | ordinal)
					cpu.Write32(where, KTrapBase | (slot << 2));		// 4-aligned: bit 0 would mean Thumb, and an ARM-state BX clears bits 1:0 (the JIT does)
				}
			}
			++o.iNumImports;
		}
	}
	return cpu.iHalt == ERunning;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// a minimal printf over the AAPCS argument registers / stack (RDebug::Printf: format in r0, arguments r1-r3 then the stack)
struct TFmt
{
	CArmCpu& iCpu;
	uint32_t iArg;
	char* iOut;
	int iMax, iN;
	void Put(char c) { if (iN < iMax - 1) iOut[iN++] = c; }
	uint32_t Arg() { const uint32_t i = iArg++; return i < 3 ? iCpu.R[1 + i] : iCpu.Read32(iCpu.R[13] + 4 * (i - 3)); }
};

static void Fmt(CArmCpu& cpu, uint32_t fmtAddr, uint32_t argIdx, char* out, int max)
{
	char fmt[256];
	cpu.CString(fmtAddr, fmt, sizeof(fmt));
	TFmt t = { cpu, argIdx, out, max, 0 };
	for (const char* s = fmt; *s; ++s)
	{
		if (*s != '%') { t.Put(*s); continue; }
		++s;
		bool left = false, zero = false;
		for (;; ++s) { if (*s == '-') left = true; else if (*s == '0') zero = true; else break; }
		int width = 0;
		while (*s >= '0' && *s <= '9') width = width * 10 + (*s++ - '0');
		while (*s == 'l' || *s == 'h') ++s;
		char tmp[40];
		int n = 0;
		switch (*s)
		{
		case '%': t.Put('%'); continue;
		case 'd': case 'i': { int32_t v = (int32_t)t.Arg(); uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v; do tmp[n++] = (char)('0' + u % 10); while (u /= 10); if (v < 0) tmp[n++] = '-'; break; }
		case 'u': { uint32_t u = t.Arg(); do tmp[n++] = (char)('0' + u % 10); while (u /= 10); break; }
		case 'x': case 'X': case 'p': { uint32_t u = t.Arg(); do { int d = u & 15; tmp[n++] = (char)(d < 10 ? '0' + d : (*s == 'X' ? 'A' : 'a') + d - 10); } while (u >>= 4); break; }
		case 'c': tmp[n++] = (char)t.Arg(); break;
		case 's':
		{
			char str[160];
			cpu.CString(t.Arg(), str, sizeof(str));
			const int l = (int)strlen(str);
			for (int k = 0; k < width - l && !left; ++k) t.Put(' ');
			for (int k = 0; k < l; ++k) t.Put(str[k]);
			for (int k = 0; k < width - l && left; ++k) t.Put(' ');
			continue;
		}
		default: t.Put('%'); t.Put(*s); continue;
		}
		for (int k = 0; k < width - n && !left; ++k) t.Put(zero ? '0' : ' ');
		const int digits = n;
		while (n) t.Put(tmp[--n]);
		for (int k = 0; k < width - digits && left; ++k) t.Put(' ');
	}
	out[t.iN] = 0;
}

void Say(THle& h, const char* fmt, const char* a, const char* b)
{
	char line[200];
	snprintf(line, sizeof(line), fmt, a, b);
	if (h.iLog) h.iLog(line, h.iLogCtx);
}

static void OnTrap(CArmCpu& cpu, uint32_t addr, void* ctx)
{
	THle& h = *(THle*)ctx;
	const uint32_t slot = (addr - KTrapBase) >> 2;
	const uint32_t v = h.iProcess ? (slot < h.iProcess->iSlotCount ? h.iProcess->iSlots[slot] : 0) : slot;
	const uint32_t dll = (v >> 16) & 0x3F, ord = v & 0xFFFF, mod = v >> 22;
	if (h.iProcess && GlTrapSlot(cpu, h, v))		// a GL function eglGetProcAddress handed out
		return;
	const TLoaded* img = h.iProcess && (int)mod < h.iProcess->iCount ? &h.iProcess->iMod[mod] : h.iImage;
	const char* dllName = img ? img->iDll[dll] : "";
	if (IsGlDll(dllName) && GlTrap(cpu, h, dllName, ord))		// OpenGL ES / EGL: the native DLLs
		return;
	if (BridgeNative(dllName) && BridgeTrap(cpu, h, dllName, ord))		// a DLL bridge.cfg runs natively (R1)
		return;
	const char* name = (strncmp(dllName, "euser", 5) == 0) ? EuserName(ord) : "";
	if (!strcmp(name, "_ZN6RDebug6PrintfEPKcz"))
	{
		char buf[200];
		Fmt(cpu, cpu.R[0], 0, buf, sizeof(buf));
		if (h.iLog) h.iLog(buf, h.iLogCtx);
		cpu.R[0] = 0;
		cpu.SetReturn();
		return;
	}
	if (!strcmp(name, "_ZN4User11InitProcessEv") || !strcmp(name, "_ZN7UserSvr9DllSetTlsEiiPv") || !strncmp(name, "_ZN8UserHeap15SetupThreadHeap", 29))
	{
		cpu.R[0] = 0;
		cpu.SetReturn();
		return;
	}
	if (!strcmp(name, "_ZN4User4ExitEi"))
	{
		cpu.Halt(EExited, (int32_t)cpu.R[0]);
		return;
	}
	++h.iUnimplemented;
	char note[120];
	snprintf(note, sizeof(note), "%s ordinal %u (%s) is not emulated", dllName, ord, name);
	Say(h, "[proteus] %s", note);
	strncpy(cpu.iNote, note, sizeof(cpu.iNote) - 1);
	cpu.Halt(EHleStop);
}

static bool OnSvc(CArmCpu& cpu, uint32_t imm, void* ctx)
{
	THle& h = *(THle*)ctx;
	if (h.iProcess && h.iProcess->iRealEuser)
		return ExecCall(cpu, h, imm);
	char n[48];
	snprintf(n, sizeof(n), "0x%x", imm);
	Say(h, "[proteus] SWI %s (executive call) is not emulated in P0", n);
	return false;
}

void InstallHle(CArmCpu& cpu, THle& h)
{
	cpu.iTrap = OnTrap;
	cpu.iSvc = OnSvc;
	cpu.iCtx = &h;
	cpu.iOnSlice = [](CArmCpu& c, void*) { Preempt(c, *(THle*)c.iCtx); };
	cpu.iSliceSteps = h.iProcess && h.iProcess->iRealEuser ? KSliceInstructions : 0;
	cpu.iNextSlice = cpu.iSteps + cpu.iSliceSteps;
}

// what the kernel does for a user thread (kernel/arm/ckernel.cia, __USERSIDE_THREAD_DATA__): the TLocalThreadData block at the very top of the
// stack, zeroed, with the thread id in it; its address in TPIDRURW; sp below it. 4 KB below the region top: our device euser's leave trace
// (euser +0x4bbc4) scans stack words upward from sp for return addresses and would run off the region.
static const uint32_t KLocalThreadDataSize = 48;		// _ALIGN_UP(sizeof(TLocalThreadData), 8): 5 words + an RArray (6 words), u32std.h
static uint32_t StartThreadStack(CArmCpu& cpu, const THle& h)
{
	const uint32_t top = (cpu.iBase + cpu.iSize - 0x1000) & ~7u, tld = top - KLocalThreadDataSize;
	for (uint32_t a = tld; a < top; a += 4) cpu.Write32(a, 0);
	cpu.Write32(tld + 12, h.iThreadId);					// TLocalThreadData::iThreadId
	cpu.iTpidrurw = tld;
	return tld;
}

void RunExe(CArmCpu& cpu, THle& h, uint64_t maxSteps)
{
	InstallHle(cpu, h);
	memset(&h.iExec, 0, sizeof(h.iExec));
	if (h.iProcess && h.iProcess->iRealEuser)		// the real euser's executive: guest threads (thread 0 = this one, the main thread)
	{
		h.iExec.iThread = (TGuestThread*)calloc(KMaxThreads, sizeof(TGuestThread));
		h.iExec.iSync = (TSyncObj*)calloc(KSyncInit, sizeof(TSyncObj));
		h.iExec.iSyncCap = KSyncInit;
		if (h.iExec.iThread && h.iExec.iSync)
		{
			h.iExec.iThreadCount = 1;
			h.iExec.iThread[0].iState = EThreadReady;
			h.iExec.iThread[0].iId = h.iThreadId ? h.iThreadId : 0x100;
			h.iExec.iThread[0].iStackChunk = -1;
			InitPriorities(h);
		}
		else { free(h.iExec.iThread); free(h.iExec.iSync); h.iExec.iThread = 0; h.iExec.iSync = 0; }
	}
	h.iExec.iLibFirstNew = h.iProcess ? h.iProcess->iCount : 0;		// entry points of the initial modules are run here, not by LibraryAttach
	for (int m = 0; h.iProcess && m < h.iProcess->iCount; ++m)
		if (h.iProcess->iMod[m].iEnd > h.iExec.iLowWater) h.iExec.iLowWater = h.iProcess->iMod[m].iEnd;
	// P2: DLL entry points first (_E32Dll, KModuleEntryReasonProcessAttach = 2: static constructors, toolchain/dll_rt/dll_entry.S and the real
	// uc_dll.cpp), each DLL after the DLLs it imports (InitOrder: load order is not that -- libc loaded before stdnew, which calls it).
	// The real euser runs them itself (User::InitProcess, after the thread heap exists: EExecStaticCallList); only the HLE needs this.
	int order[KMaxModules];
	const int nInit = h.iProcess && !h.iProcess->iRealEuser ? InitOrder(*h.iProcess, 0, 1, order) : 0;
	for (int k = 0; k < nInit; ++k)
	{
		const TLoaded& d = h.iProcess->iMod[order[k]];
		// euser is never called (the kernel skips it); KImageNoCallEntryPoint; our device builds of the system DLLs (build_*_device.py) have
		// entry offset 0 on ordinary code -- an entry at offset 0 is only real when it is our dll_entry.S stub (word 3 = the UID3)
		if (!strcmp(d.iName, "euser.dll") || (d.iFlags & 2) || (d.iEntry == d.iLoadAddr && cpu.Read32(d.iEntry + 12) != d.iUid3))
			continue;
		memset(cpu.R, 0, sizeof(cpu.R));
		cpu.R[0] = 2;
		cpu.R[13] = StartThreadStack(cpu, h);
		cpu.R[14] = KReturnToHost;
		cpu.R[15] = d.iEntry;
		cpu.Run(maxSteps);
		if (cpu.iHalt != EReturned || cpu.iExitCode != 0)
		{
			char n[120];
			snprintf(n, sizeof(n), "%s entry point: halt %d result %d", d.iName, (int)cpu.iHalt, (int)cpu.iExitCode);
			Say(h, "[proteus] %s", n);
			if (cpu.iHalt == EReturned) cpu.iHalt = EFault;		// (a returned entry with an error: stop the process)
			return;
		}
		cpu.iHalt = ERunning;
	}
	memset(cpu.R, 0, sizeof(cpu.R));
	cpu.R[13] = StartThreadStack(cpu, h) - 64;			// below the thread data: the creation info (64 bytes, iTotalSize)
	// the kernel starts a thread with its SStdEpocThreadCreateInfo at sp (_E32Startup hands sp to UserHeap::SetupThreadHeap): u32std.h layout
	{
		const uint32_t ci = cpu.R[13];
		for (int i = 0; i < 16; ++i) cpu.Write32(ci + 4 * i, 0);
		cpu.Write32(ci + 24, (cpu.iBase + cpu.iSize - KStackBytes) & ~0xFFFu);	// iUserStack
		cpu.Write32(ci + 28, KStackBytes);							// iUserStackSize
		cpu.Write32(ci + 44, 64);									// iTotalSize
		cpu.Write32(ci + 52, h.iImage->iHeapMin ? h.iImage->iHeapMin : 0x1000);	// iHeapInitialSize
		cpu.Write32(ci + 56, h.iImage->iHeapMax ? h.iImage->iHeapMax : 0x100000);	// iHeapMaxSize
	}
	cpu.R[4] = 0;											// reason: first thread
	cpu.R[14] = KReturnToHost;
	cpu.R[15] = h.iImage->iEntry;
	cpu.Run(maxSteps);
}
}

namespace proteus
{
// "module+0xoffset (nearest export: ordinal N[ name] +0xdelta)" for a guest address (halt reports)
void Where(const TProcess& aProc, CArmCpu& aCpu, uint32_t aAddr, char* aOut, int aMax)
{
	const uint32_t a = aAddr & ~1u;
	for (int m = 0; m < aProc.iCount; ++m)
	{
		const TLoaded& l = aProc.iMod[m];
		if (a < l.iLoadAddr || a >= l.iEnd) continue;
		uint32_t best = 0, bestOrd = 0;
		for (uint32_t i = 0; i < l.iExportCount; ++i)
		{
			const uint32_t e = aCpu.Read32(l.iExportDir + 4 * i) & ~1u;
			if (e <= a && e > best) { best = e; bestOrd = i + 1; }
		}
		const char* nm = bestOrd && !strcmp(l.iName, "euser.dll") ? EuserName(bestOrd) : "";
		if (bestOrd)
			snprintf(aOut, aMax, "%s+0x%x (export %u %s +0x%x)", m ? l.iName : "exe", a - l.iLoadAddr, bestOrd, nm, a - best);
		else
			snprintf(aOut, aMax, "%s+0x%x", m ? l.iName : "exe", a - l.iLoadAddr);
		return;
	}
	snprintf(aOut, aMax, "0x%x (no module)", aAddr);
}
}

namespace proteus
{
// P2: RLibrary::Load in the guest -- load an ARM DLL (and the DLLs it imports that are not loaded yet) after the highest loaded module.
// Returns the module index (an already loaded DLL: its index) or an error; aFirstNew = the first new module index (== iCount: none new).
int LoadLibraryModule(CArmCpu& aCpu, TProcess& aProc, const char* aName, TFindDllFn aFind, void* aFindCtx, int& aFirstNew)
{
	char base[40];
	BaseName(aName, base, sizeof(base));
	aFirstNew = aProc.iCount;
	for (int m = 1; m < aProc.iCount; ++m)
		if (!strcmp(aProc.iMod[m].iName, base)) return m;
	if (aProc.iCount >= KMaxModules) return -4;				// KErrNoMemory
	uint32_t len = 0;
	char drive = 0;
	const uint8_t* img = aFind ? aFind(base, len, drive, aFindCtx) : NULL;
	if (!img) return -1;									// KErrNotFound
	if (len < 4 || U32(img) != 0x10000079) return -20;		// KErrCorrupt-ish: not a DLL
	uint32_t next = 0;
	for (int k = 0; k < aProc.iCount; ++k) if (aProc.iMod[k].iEnd > next) next = aProc.iMod[k].iEnd;
	const int m = aProc.iCount++;
	strncpy(aProc.iMod[m].iName, base, sizeof(aProc.iMod[m].iName) - 1);
	aProc.iMod[m].iDrive = drive;
	if (!LoadE32At(aCpu, img, len, next, aProc.iMod[m], &aProc, aFind, aFindCtx))
	{
		strncpy(aProc.iError, aProc.iMod[m].iError, sizeof(aProc.iError) - 1);
		return -20;
	}
	return m;
}

// should the loader call this module's entry point? (the same rules as RunExe)
bool CallsEntry(CArmCpu& aCpu, const TLoaded& d)
{
	return strcmp(d.iName, "euser.dll") && !(d.iFlags & 2) && !(d.iEntry == d.iLoadAddr && aCpu.Read32(d.iEntry + 12) != d.iUid3);
}

static void Visit(const TProcess& p, int m, int first, bool* seen, int* order, int& n)
{
	seen[m] = true;
	const TLoaded& d = p.iMod[m];
	for (int i = 0; i < d.iNumImports && i < KMaxDllRefs; ++i)
	{
		const int dep = d.iDllModule[i];
		if (dep >= first && dep < p.iCount && !seen[dep])
			Visit(p, dep, first, seen, order, n);
	}
	if (m >= first)
		order[n++] = m;
}

int InitOrder(const TProcess& aProc, int aRoot, int aFirst, int* aOrder)
{
	bool seen[KMaxModules] = {};
	int n = 0;
	Visit(aProc, aRoot, aFirst, seen, aOrder, n);
	for (int m = aFirst; m < aProc.iCount; ++m)		// (a new module nothing reachable imports: still initialised, last)
		if (!seen[m])
			Visit(aProc, m, aFirst, seen, aOrder, n);
	return n;
}
}

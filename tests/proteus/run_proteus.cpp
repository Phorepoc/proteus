// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// tests/proteus/run_proteus.cpp -- host test of the Proteus P0 interpreter (src/proteus): loads a real ARM Symbian EXE (default emitter/proteushello.exe, built by ./ph arm
// proteushello), runs it from its E32 entry point and checks the RDebug::Printf line + exit. Build/run: bash tests/proteus/run.sh
#include "../../os/src/proteus/proteusrt.h"
#include "../../os/src/proteus/e32decomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

static char gLines[16][200];
static int gN;
// P2: ARM DLLs are looked up next to the EXE (emitter/<name>)
static std::string gDir;
static std::vector<std::vector<uint8_t> > gDlls;
static const uint8_t* FindDll(const char* name, uint32_t& len, char& drive, void*)
{
	FILE* f = fopen((gDir + name).c_str(), "rb");
	if (!f && getenv("PROTEUS_DLLDIR")) f = fopen((std::string(getenv("PROTEUS_DLLDIR")) + "/" + name).c_str(), "rb");	// e.g. emitter (our ARM builds)
	if (!f) return 0;
	gDlls.push_back(std::vector<uint8_t>());
	std::vector<uint8_t>& v = gDlls.back();
	uint8_t b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
	fclose(f);
	printf("  (DLL %s%s: %u bytes)\n", gDir.c_str(), name, (unsigned)v.size());
	len = (uint32_t)v.size();
	return v.data();
}

static void Log(const char* l, void*) { printf("  | %s\n", l); if (gN < 16) strncpy(gLines[gN++], l, 199); }

static std::vector<uint8_t> ReadAll(const char* path)
{
	std::vector<uint8_t> v;
	FILE* f = fopen(path, "rb");
	if (!f) return v;
	uint8_t b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
	fclose(f);
	return v;
}

// --decomp <compressed e32> <raw reference (emitter/e32image.py decompress_e32)>: the C++ decoder must give the same bytes
static int DecompCheck(const char* in, const char* ref)
{
	std::vector<uint8_t> a = ReadAll(in), r = ReadAll(ref);
	uint32_t n = 0;
	const char* err = "";
	uint8_t* raw = proteus::DecompressE32(a.data(), (uint32_t)a.size(), n, err);
	const bool ok = raw && n == r.size() && memcmp(raw, r.data(), n) == 0;
	printf("decomp %s: %u -> %u bytes (ref %u) %s %s\n", in, (unsigned)a.size(), n, (unsigned)r.size(), err, ok ? "PASS" : "FAIL");
	free(raw);
	return ok ? 0 : 1;
}

// --thumb: the Thumb-1 decoder (src/proteus/thumb.cpp) on a hand-assembled snippet (arm-none-eabi-as -march=armv5t, linked at 0x1000):
// ARM entry -> BLX imm -> Thumb: formats 1-6, 8-15, BL pair, BLX reg / BLX imm back to ARM, B<cond>, NEG/MVN, POP {pc} back to ARM.
// Returns 42 in r0 on success (1 = a check inside failed). Source: the comment block below.
static const uint32_t KThumbSnippet[] =
{
	0xe92d4010, 0xfa000000, 0xe8bd8010, 0x200ab5f0, 0x18422103, 0x00933a01, 0x105b085b, 0x2b904353, 0x4c1ad12e, 0xb084466d, 0x9e019401,
	0xd12742a6, 0x2055af02, 0x78797078, 0x887a807c, 0x42824814, 0x2002d11e, 0x48125e3a, 0xd1194282, 0x21071c38, 0xc0062209, 0xc8063808,
	0x29101889, 0xb004d110, 0xf812f000, 0xd10b2863, 0x4788490a, 0xd107284d, 0xe812f000, 0xd103284d, 0x42412000, 0xd40143c9, 0xbdf02001,
	0xbdf0202a, 0x47702063, 0x12345678, 0x00005678, 0x00001098, 0xe3a0004d, 0xe12fff1e
};
/* arm_entry: push {r4,lr}; blx thumb_main; pop {r4,pc}
   thumb_main: push {r4-r7,lr}; movs r0,#10; movs r1,#3; adds r2,r0,r1; subs r2,r2,#1; lsls r3,r2,#2; lsrs r3,r3,#1; asrs r3,r3,#1; muls r3,r2;
   cmp r3,#144; bne fail; ldr r4,=0x12345678; mov r5,sp; sub sp,#16; str r4,[sp,#4]; ldr r6,[sp,#4]; cmp r6,r4; bne fail; add r7,sp,#8;
   movs r0,#0x55; strb r0,[r7,#1]; ldrb r1,[r7,#1]; strh r4,[r7,#2]; ldrh r2,[r7,#2]; ldr r0,=0x5678; cmp r2,r0; bne fail; movs r0,#2;
   ldrsh r2,[r7,r0]; ldr r0,=0x5678; cmp r2,r0; bne fail; adds r0,r7,#0; movs r1,#7; movs r2,#9; stmia r0!,{r1,r2}; subs r0,#8;
   ldmia r0!,{r1,r2}; adds r1,r1,r2; cmp r1,#16; bne fail; add sp,#16; bl helper; cmp r0,#99; bne fail; ldr r1,=arm_helper; blx r1;
   cmp r0,#77; bne fail; blx arm_helper; cmp r0,#77; bne fail; movs r0,#0; negs r1,r0; mvns r1,r1; bmi ok (0xFFFFFFFF: N)
   fail: movs r0,#1; pop {r4-r7,pc}    ok: movs r0,#42; pop {r4-r7,pc}    helper: movs r0,#99; bx lr    arm_helper (ARM): mov r0,#77; bx lr */
static int ThumbTest()
{
	proteus::CArmCpu cpu;
	std::vector<uint8_t> mem(1 << 16);
	cpu.iHost = mem.data();
	cpu.iBase = 0x1000;
	cpu.iSize = (uint32_t)mem.size();
	memcpy(mem.data(), KThumbSnippet, sizeof(KThumbSnippet));
	cpu.R[13] = cpu.iBase + cpu.iSize - 16;
	cpu.R[14] = proteus::KReturnToHost;
	cpu.R[15] = 0x1000;
	if (getenv("PROTEUS_TRACE"))
		while (cpu.iHalt == proteus::ERunning && cpu.iSteps < 10000)
		{
			printf("  %c %04x  r0=%x r1=%x r2=%x r3=%x r4=%x r6=%x r7=%x sp=%x lr=%x NZCV=%u%u%u%u\n", cpu.iT ? 'T' : 'A', cpu.R[15], cpu.R[0], cpu.R[1], cpu.R[2], cpu.R[3],
				cpu.R[4], cpu.R[6], cpu.R[7], cpu.R[13], cpu.R[14], cpu.iN, cpu.iZ, cpu.iC, cpu.iV);
			cpu.Run(1);
			if (cpu.iHalt == proteus::EStepLimit) cpu.iHalt = proteus::ERunning;
		}
	cpu.Run(10000);
	const bool ok = cpu.iHalt == proteus::EReturned && cpu.R[0] == 42 && cpu.iT == 0;
	printf("thumb: halt %d r0 %u T %u steps %llu note '%s' pc %#x -> %s\n", (int)cpu.iHalt, cpu.R[0], cpu.iT, (unsigned long long)cpu.iSteps, cpu.iNote, cpu.iLastPc, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "--thumb")) return ThumbTest();
	if (argc > 3 && !strcmp(argv[1], "--decomp")) return DecompCheck(argv[2], argv[3]);
	const char* path = argc > 1 ? argv[1] : "emitter/proteushello.exe";
	const char* want = argc > 2 ? argv[2] : "PH: proteus hello from ARM code: 40 + 2 = 42, ok";
	FILE* f = fopen(path, "rb");
	if (!f) { printf("cannot open %s\n", path); return 2; }
	std::vector<uint8_t> img;
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) img.insert(img.end(), buf, buf + n);
	fclose(f);
	proteus::CArmCpu cpu;
	std::vector<uint8_t> mem(32 << 20);
	cpu.iHost = mem.data();
	cpu.iBase = 0x00400000;
	cpu.iSize = (uint32_t)mem.size();
	gDlls.reserve(16);
	const char* slash = strrchr(path, '/');
	gDir = slash ? std::string(path, slash + 1 - path) : std::string();
	static proteus::TProcess proc;
	proc.iRealEuser = getenv("PROTEUS_REAL_EUSER") != 0;		// euser.dll = a real ARM euser found like any DLL (emitter/euser_dev.dll copied as euser.dll)
	if (!proteus::LoadProcess(cpu, img.data(), (uint32_t)img.size(), cpu.iBase + 0x1000, FindDll, 0, proc)) { printf("LOAD FAILED: %s\n", proc.iError); return 1; }
	proteus::TLoaded& ld = proc.iMod[0];
	for (int m = 1; m < proc.iCount; ++m) printf("module %d: %s at %#x exports %u\n", m, proc.iMod[m].iName, proc.iMod[m].iLoadAddr, proc.iMod[m].iExportCount);
	printf("loaded %s: cpu %#x uid3 %#x at %#x entry %#x delta %#x imports %d (%s)\n", path, ld.iCpu, ld.iUid3, ld.iLoadAddr, ld.iEntry, ld.iDelta, ld.iNumImports, ld.iDll[0]);
	proteus::THle hle = { &ld, &proc, Log, 0, 0 };
	proteus::RunExe(cpu, hle, 500000000);
	printf("halt %d exit %d steps %llu fault %#x note '%s' unimplemented %d\n", (int)cpu.iHalt, cpu.iExitCode, (unsigned long long)cpu.iSteps, cpu.iFaultAddr, cpu.iNote, hle.iUnimplemented);
	if (cpu.iHalt != proteus::EExited)
	{
		printf("regs:");
		for (int i = 0; i < 16; ++i) printf(" r%d=%x", i, cpu.R[i]);
		printf("\n");
		for (int m = 0; m < proc.iCount; ++m)
			if (cpu.R[15] >= proc.iMod[m].iLoadAddr && cpu.R[15] < proc.iMod[m].iEnd)
				printf("pc in module %d %s +%#x (lr +%#x)\n", m, proc.iMod[m].iName, cpu.R[15] - proc.iMod[m].iLoadAddr, cpu.R[14] - proc.iMod[m].iLoadAddr);
	}
	bool ok = cpu.iHalt == proteus::EExited && cpu.iExitCode == 0;
	bool saw = false;
	for (int i = 0; i < gN; ++i) saw |= strstr(gLines[i], want) != 0;
	printf(ok && saw ? "PASS\n" : "FAIL\n");
	return ok && saw ? 0 : 1;
}

// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
/* vfpvec.c -- VFPv2 short vectors (FPSCR.LEN / FPSCR.STRIDE) on armcpu.cpp. Self-checking: qemu-arm's Cortex-A15 has no short vectors (ARMv8 and
 * VFPv4 made LEN/STRIDE RES0), so there is no reference to diff against; the expectations are the ARM ARM (VFPv2) rules worked out by hand:
 * a destination outside the first bank (s0-s7, d0-d3) makes the operation a vector of LEN elements stepping STRIDE inside the bank (wrapping),
 * a source Fm in the first bank is the same scalar for every element. Prints "vfpvec PASS" or "vfpvec FAIL <case>".
 *   arm-none-eabi-gcc -marm -march=armv7ve -mfpu=vfpv3 -mfloat-abi=softfp -O1 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -Wl,-e,_start -o vfpvec.elf vfpvec.c
 *   harness vfpvec.elf
 */
typedef unsigned int u32;

static void sys_write(const char* p, u32 n)
{
	register u32 r0 asm("r0") = 1, r1 asm("r1") = (u32)p, r2 asm("r2") = n, r7 asm("r7") = 4;
	asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}

static void sys_exit(u32 code)
{
	register u32 r0 asm("r0") = code, r7 asm("r7") = 1;
	asm volatile("svc #0" : : "r"(r0), "r"(r7));
	for (;;) ;
}

static const float A[4] = { 1, 2, 3, 4 }, B[4] = { 10, 20, 30, 40 }, C[4] = { 100, 200, 300, 400 };
static const double DA[2] = { 1.5, 2.5 }, DB[2] = { 10, 20 };
static float R[8];
static double DR[4];
static u32 fp;

static int fails;
static void check(const char* name, int ok)
{
	if (ok) return;
	++fails;
	sys_write("vfpvec FAIL ", 12);
	int n = 0; while (name[n]) ++n;
	sys_write(name, n);
	sys_write("\n", 1);
}

#define CLOB "r12", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15", "s16", "s17", "s18", "s19", \
	"s20", "s21", "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory"

void _start(void)
{
	/* A: vmul.f32 s24, s8, s16 with LEN=4: s24..s27 = A * B (element by element) */
	asm volatile("vldmia %1, {s8-s11}\n\tvldmia %2, {s16-s19}\n\t"
		"vmrs r12, fpscr\n\torr r12, r12, #0x30000\n\tvmsr fpscr, r12\n\t"
		"vmul.f32 s24, s8, s16\n\t"
		"vmrs %0, fpscr\n\tbic r12, r12, #0x70000\n\tvmsr fpscr, r12\n\t"
		"vstmia %3, {s24-s27}" : "=&r"(fp) : "r"(A), "r"(B), "r"(R) : CLOB);
	check("A mul", R[0] == 10 && R[1] == 40 && R[2] == 90 && R[3] == 160);
	check("A fpscr LEN kept", ((fp >> 16) & 7) == 3);

	/* B: Fm in the first bank is a scalar: vmul.f32 s24, s8, s2 (s2 = 2) -> s24..s27 = A * 2 */
	{
		static const float two[1] = { 2 };
		asm volatile("vldmia %0, {s8-s11}\n\tvldr s2, [%1]\n\t"
			"vmrs r12, fpscr\n\torr r12, r12, #0x30000\n\tvmsr fpscr, r12\n\t"
			"vmul.f32 s24, s8, s2\n\t"
			"bic r12, r12, #0x70000\n\tvmsr fpscr, r12\n\tvstmia %2, {s24-s27}" : : "r"(A), "r"(two), "r"(R) : CLOB);
		check("B scalar Fm", R[0] == 2 && R[1] == 4 && R[2] == 6 && R[3] == 8);
	}

	/* C: a destination whose series wraps inside its bank: vadd.f32 s14, s16, s20 -> s14, s15, s8, s9 */
	asm volatile("vldmia %0, {s16-s19}\n\tvldmia %1, {s20-s23}\n\t"
		"vmrs r12, fpscr\n\torr r12, r12, #0x30000\n\tvmsr fpscr, r12\n\t"
		"vadd.f32 s14, s16, s20\n\t"
		"bic r12, r12, #0x70000\n\tvmsr fpscr, r12\n\tvstmia %2!, {s14-s15}\n\tvstmia %2, {s8-s9}" : : "r"(B), "r"(C), "r"(R) : CLOB);
	check("C wrap", R[0] == 110 && R[1] == 220 && R[2] == 330 && R[3] == 440);

	/* D: STRIDE 2, LEN 2: vadd.f32 s8, s16, s24 -> s8, s10 = (s16, s18) + (s24, s26) */
	asm volatile("vldmia %0, {s16-s19}\n\tvldmia %1, {s24-s27}\n\t"
		"vmrs r12, fpscr\n\torr r12, r12, #0x310000\n\tvmsr fpscr, r12\n\t"
		"vadd.f32 s8, s16, s24\n\t"
		"bic r12, r12, #0x370000\n\tvmsr fpscr, r12\n\tvstr s8, [%2]\n\tvstr s10, [%2, #4]" : : "r"(B), "r"(A), "r"(R) : CLOB);
	check("D stride2", R[0] == 11 && R[1] == 33);

	/* E: double precision LEN=2: vadd.f64 d4, d8, d12 -> d4, d5 */
	asm volatile("vldmia %0, {d8-d9}\n\tvldmia %1, {d12-d13}\n\t"
		"vmrs r12, fpscr\n\torr r12, r12, #0x10000\n\tvmsr fpscr, r12\n\t"
		"vadd.f64 d4, d8, d12\n\t"
		"bic r12, r12, #0x70000\n\tvmsr fpscr, r12\n\tvstmia %2, {d4-d5}" : : "r"(DA), "r"(DB), "r"(DR) : "r12", "d4", "d5", "d8", "d9", "d12", "d13", "memory");
	check("E double", DR[0] == 11.5 && DR[1] == 22.5);

	/* F: unary, vector destination: vneg.f32 s8, s16 (LEN=4) -> s8..s11 = -B; and the destination in bank 0 stays scalar */
	asm volatile("vldmia %0, {s16-s19}\n\t"
		"vmrs r12, fpscr\n\torr r12, r12, #0x30000\n\tvmsr fpscr, r12\n\t"
		"vneg.f32 s8, s16\n\tvmov.f32 s1, s17\n\t"
		"bic r12, r12, #0x70000\n\tvmsr fpscr, r12\n\tvstmia %1, {s8-s11}\n\tvstr s1, [%2]" : : "r"(B), "r"(R), "r"(&R[4]) : CLOB);
	check("F neg vector", R[0] == -10 && R[1] == -20 && R[2] == -30 && R[3] == -40);
	check("F bank0 scalar", R[4] == 20);

	/* G: vmla accumulates into each destination element: s8..s11 = (s8..s11) + (s16..s19) * (s24..s27) */
	asm volatile("vldmia %0, {s8-s11}\n\tvldmia %1, {s16-s19}\n\tvldmia %2, {s24-s27}\n\t"
		"vmrs r12, fpscr\n\torr r12, r12, #0x30000\n\tvmsr fpscr, r12\n\t"
		"vmla.f32 s8, s16, s24\n\t"
		"bic r12, r12, #0x70000\n\tvmsr fpscr, r12\n\tvstmia %3, {s8-s11}" : : "r"(A), "r"(B), "r"(C), "r"(R) : CLOB);
	check("G mla", R[0] == 1001 && R[1] == 4002 && R[2] == 9003 && R[3] == 16004);

	if (!fails) sys_write("vfpvec PASS\n", 12);
	sys_exit(fails ? 1 : 0);
}

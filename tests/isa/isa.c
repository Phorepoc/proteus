// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
/* isa.c -- the Proteus instruction differential test: every ARMv6/v7 ARM-mode instruction the interpreter gained beyond ARMv5TE, over
 * pseudo-random inputs (and the edge values), one hex line per result (+ APSR where an instruction sets flags or GE). Freestanding: Linux
 * EABI write/exit system calls only, so the same ELF runs under qemu-arm (the reference) and under armcpu.cpp (harness.cpp).
 * Build: arm-none-eabi-gcc -marm -march=armv7ve -O1 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -o isa.elf isa.c
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

static char buf[64];
static void out(const char* name, u32 a, u32 b, u32 r, u32 f)
{
	static const char hex[] = "0123456789abcdef";
	u32 n = 0, v[4] = { a, b, r, f };
	while (*name) buf[n++] = *name++;
	for (int k = 0; k < 4; ++k) { buf[n++] = ' '; for (int i = 28; i >= 0; i -= 4) buf[n++] = hex[(v[k] >> i) & 15]; }
	buf[n++] = '\n';
	sys_write(buf, n);
}

static u32 seed = 0x12345678;
static u32 rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	const u32 k = seed >> 28;		/* one input in four is an edge value */
	if (k == 0) return 0;
	if (k == 1) return 0xFFFFFFFFu;
	if (k == 2) return 0x80000000u;
	if (k == 3) return 0x7FFFFFFFu ^ (seed & 0x00FF00FF);
	return seed ^ (seed << 7);
}

#define N 48
/* Rd = op(Rn, Rm) */
#define T2(insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), b = rnd(), r; \
	asm volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); out(insn, a, b, r, 0); } } while (0)
/* Rd = op(Rn, Rm), APSR afterwards (GE): clear GE first */
#define T2G(insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), b = rnd(), r, f; \
	asm volatile("msr APSR_nzcvqg, %4\n\t" insn " %0, %2, %3\n\tmrs %1, APSR" : "=&r"(r), "=&r"(f) : "r"(a), "r"(b), "r"(0)); out(insn, a, b, r, f & 0xF80F0000); } } while (0)
/* Rd = op(Rm) with a fixed suffix (rotation, saturation...) */
#define T1(name, insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), r; \
	asm volatile(insn : "=r"(r) : "r"(a)); out(name, a, 0, r, 0); } } while (0)
/* Rd = op(Rn, Rm) with an operand template */
#define TX(name, insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), b = rnd(), r; \
	asm volatile(insn : "=r"(r) : "r"(a), "r"(b)); out(name, a, b, r, 0); } } while (0)
/* Rd = op(Rn, Rm, Ra) */
#define T3(name, insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), b = rnd(), c = rnd(), r; \
	asm volatile(insn : "=r"(r) : "r"(a), "r"(b), "r"(c)); out(name, a, b, r, c); } } while (0)
/* RdHi:RdLo = op(Rn, Rm, RdHi:RdLo) */
#define TL(name, insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), b = rnd(), lo = rnd(), hi = rnd(); \
	asm volatile(insn : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); out(name, a, b, lo, hi); } } while (0)

void _start(void)
{
	/* parallel add/subtract: the modular ones set GE */
	T2G("sadd16"); T2G("sasx"); T2G("ssax"); T2G("ssub16"); T2G("sadd8"); T2G("ssub8");
	T2G("uadd16"); T2G("uasx"); T2G("usax"); T2G("usub16"); T2G("uadd8"); T2G("usub8");
	T2("qadd16"); T2("qasx"); T2("qsax"); T2("qsub16"); T2("qadd8"); T2("qsub8");
	T2("uqadd16"); T2("uqasx"); T2("uqsax"); T2("uqsub16"); T2("uqadd8"); T2("uqsub8");
	T2("shadd16"); T2("shasx"); T2("shsax"); T2("shsub16"); T2("shadd8"); T2("shsub8");
	T2("uhadd16"); T2("uhasx"); T2("uhsax"); T2("uhsub16"); T2("uhadd8"); T2("uhsub8");
	/* SEL after UADD8 / USUB8 / SSUB16 (the GE bits a strlen/memchr loop uses) */
	TX("sel/uadd8", "uadd8 %0, %1, %2\n\tsel %0, %1, %2");
	TX("sel/usub8", "usub8 %0, %1, %2\n\tsel %0, %2, %1");
	TX("sel/ssub16", "ssub16 %0, %1, %2\n\tsel %0, %1, %2");
	/* packing, extends, reversal, saturation */
	TX("pkhbt0", "pkhbt %0, %1, %2"); TX("pkhbt9", "pkhbt %0, %1, %2, lsl #9");
	TX("pkhtb5", "pkhtb %0, %1, %2, asr #5"); TX("pkhtb32", "pkhtb %0, %1, %2, asr #32");
	T1("sxtb", "sxtb %0, %1"); T1("sxtb8", "sxtb %0, %1, ror #8"); T1("sxth", "sxth %0, %1"); T1("sxth16", "sxth %0, %1, ror #16");
	T1("uxtb", "uxtb %0, %1"); T1("uxtb24", "uxtb %0, %1, ror #24"); T1("uxth", "uxth %0, %1"); T1("uxth8", "uxth %0, %1, ror #8");
	T1("sxtb16", "sxtb16 %0, %1"); T1("uxtb16r8", "uxtb16 %0, %1, ror #8");
	TX("sxtab", "sxtab %0, %1, %2"); TX("sxtah", "sxtah %0, %1, %2, ror #16"); TX("uxtab", "uxtab %0, %1, %2, ror #8");
	TX("uxtah", "uxtah %0, %1, %2"); TX("sxtab16", "sxtab16 %0, %1, %2"); TX("uxtab16", "uxtab16 %0, %1, %2, ror #16");
	T1("rev", "rev %0, %1"); T1("rev16", "rev16 %0, %1"); T1("revsh", "revsh %0, %1"); T1("rbit", "rbit %0, %1");
	T1("ssat8", "ssat %0, #8, %1"); T1("ssat16a", "ssat %0, #16, %1, asr #3"); T1("ssat1", "ssat %0, #1, %1"); T1("ssat32", "ssat %0, #32, %1, lsl #4");
	T1("usat8", "usat %0, #8, %1"); T1("usat15", "usat %0, #15, %1, lsl #2"); T1("usat0", "usat %0, #0, %1"); T1("usat31", "usat %0, #31, %1, asr #32");
	T1("ssat16x", "ssat16 %0, #9, %1"); T1("usat16x", "usat16 %0, #7, %1");
	/* bitfields */
	T1("ubfx", "ubfx %0, %1, #5, #11"); T1("ubfx32", "ubfx %0, %1, #0, #32"); T1("sbfx", "sbfx %0, %1, #3, #9"); T1("sbfx1", "sbfx %0, %1, #31, #1");
	TX("bfi", "mov %0, %1\n\tbfi %0, %2, #7, #13"); T1("bfc", "mov %0, %1\n\tbfc %0, #4, #20");
	/* MOVW / MOVT */
	T1("movw", "movw %0, #0xbeef\n\teor %0, %0, %1"); T1("movt", "mov %0, %1\n\tmovt %0, #0xcafe");
	/* multiplies, divides */
	T3("mls", "mls %0, %1, %2, %3");
	TL("umaal", "umaal %0, %1, %2, %3");
	T3("smlad", "smlad %0, %1, %2, %3"); T3("smladx", "smladx %0, %1, %2, %3"); T3("smlsd", "smlsd %0, %1, %2, %3"); T3("smlsdx", "smlsdx %0, %1, %2, %3");
	TX("smuad", "smuad %0, %1, %2"); TX("smusdx", "smusdx %0, %1, %2");
	TL("smlald", "smlald %0, %1, %2, %3"); TL("smlsldx", "smlsldx %0, %1, %2, %3");
	TX("smmul", "smmul %0, %1, %2"); TX("smmulr", "smmulr %0, %1, %2");
	T3("smmla", "smmla %0, %1, %2, %3"); T3("smmlsr", "smmlsr %0, %1, %2, %3");
	TX("usad8", "usad8 %0, %1, %2"); T3("usada8", "usada8 %0, %1, %2, %3");
	TX("sdiv", "sdiv %0, %1, %2"); TX("udiv", "udiv %0, %1, %2");
	{ u32 r; asm volatile("sdiv %0, %1, %2" : "=r"(r) : "r"(0x80000000u), "r"(0xFFFFFFFFu)); out("sdivmin", 0x80000000u, 0xFFFFFFFFu, r, 0); }
	{ u32 r; asm volatile("udiv %0, %1, %2" : "=r"(r) : "r"(1234u), "r"(0u)); out("udiv0", 1234u, 0, r, 0); }
	/* the user read/write thread ID register (EKA2's TLocalThreadData pointer) */
	T1("tpidrurw", "mcr p15, 0, %1, c13, c0, 2\n\tmov %0, #0\n\tmrc p15, 0, %0, c13, c0, 2");
	/* exclusives: the doubleword pair */
	{
		static unsigned long long d = 0x1122334455667788ull;
		u32 lo, hi, st;
		asm volatile("ldrexd %0, %H0, [%2]" : "=&r"(*(unsigned long long*)buf) : "m"(d), "r"(&d));
		lo = ((u32*)buf)[0]; hi = ((u32*)buf)[1];
		unsigned long long v = 0xA1B2C3D4E5F60718ull;
		asm volatile("ldrexd r4, r5, [%2]\n\tstrexd %0, %3, %H3, [%2]" : "=&r"(st), "+m"(d) : "r"(&d), "r"(v) : "r4", "r5");
		out("ldrexd", lo, hi, st, (u32)(d >> 32) ^ (u32)d);
	}
	sys_exit(0);
}

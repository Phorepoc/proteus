// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
/* base.c -- the second Proteus instruction differential test (run.sh: qemu-arm vs armcpu.cpp, identical output = PASS). isa.c covers what ARMv6/v7
 * added; this covers the base ARM instruction classes the table-driven fast path (armfast.inc) executes: all 16 data-processing operations x S x
 * 17 operand forms with the flags going in and out, LDR/STR/LDRB/STRB in every addressing mode (immediate, register + shift, offset / pre / post
 * index, add / subtract), halfword and signed loads/stores, LDM/STM in the four modes with and without write-back, every condition over every
 * NZCV, MUL/MLA/long multiplies with S, BX/BLX/BL (incl. an ARM <-> Thumb round trip), LDREX/STREX, MOVW/MOVT, and pc as an operand.
 * Freestanding (Linux EABI write/exit only), like isa.c. No unaligned word accesses (ARMv5 rotates, ARMv7 does not: a known, deliberate difference). */
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
static char buf[96];
static void out(const char* name, u32 a, u32 b, u32 r, u32 f)
{
	static const char hex[] = "0123456789abcdef";
	u32 n = 0, v[4] = { a, b, r, f };
	while (*name && n < 60) buf[n++] = *name++;
	for (int k = 0; k < 4; ++k) { buf[n++] = ' '; for (int i = 28; i >= 0; i -= 4) buf[n++] = hex[(v[k] >> i) & 15]; }
	buf[n++] = '\n';
	sys_write(buf, n);
}
static u32 seed = 0x2468ace1;
static u32 rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	const u32 k = seed >> 28;
	if (k == 0) return 0;
	if (k == 1) return 0xFFFFFFFFu;
	if (k == 2) return 0x80000000u;
	if (k == 3) return 0x7FFFFFFFu;
	if (k == 4) return 1;
	return seed ^ (seed << 7);
}
static u32 shamt(void)
{
	static const u32 e[] = { 0, 1, 31, 32, 33, 63, 64, 255, 256, 0x101 };
	const u32 v = rnd();
	return (v & 3) == 0 ? e[(v >> 8) % 10] : (v >> 4) & 0x3F;
}

#define N 6
/* one data-processing instruction template: %0 = Rd (dest, seeded), %1 = flags out, %2 = Rn, %3 = Rm / value, %4 = flags in, %5 = shift amount */
#define ALU(name, insn) do { for (int i = 0; i < N; ++i) { u32 a = rnd(), b = rnd(), fi = rnd() & 0xF0000000u, sh = shamt(), r = 0x5a5a5a5a, fo; \
	asm volatile("msr APSR_nzcvq, %4\n\t" insn "\n\tmrs %1, APSR" : "+r"(r), "=&r"(fo) : "r"(a), "r"(b), "r"(fi), "r"(sh) : "cc"); \
	out(name, a ^ (sh << 8), b, r, (fo & 0xF0000000u) | (fi >> 4)); } } while (0)
#define O2(op, pre, name) \
	ALU(name "i1", op " " pre "#0xff"); ALU(name "i2", op " " pre "#0x3fc"); ALU(name "i3", op " " pre "#0xff000000"); ALU(name "i4", op " " pre "#0xc000003f"); \
	ALU(name "r", op " " pre "%3"); ALU(name "lsl5", op " " pre "%3, lsl #5"); ALU(name "lsl31", op " " pre "%3, lsl #31"); \
	ALU(name "lsr7", op " " pre "%3, lsr #7"); ALU(name "lsr32", op " " pre "%3, lsr #32"); ALU(name "asr9", op " " pre "%3, asr #9"); \
	ALU(name "asr32", op " " pre "%3, asr #32"); ALU(name "ror13", op " " pre "%3, ror #13"); ALU(name "rrx", op " " pre "%3, rrx"); \
	ALU(name "lslR", op " " pre "%3, lsl %5"); ALU(name "lsrR", op " " pre "%3, lsr %5"); ALU(name "asrR", op " " pre "%3, asr %5"); ALU(name "rorR", op " " pre "%3, ror %5")
#define OP3(op) O2(#op, "%0, %2, ", #op); O2(#op "s", "%0, %2, ", #op "s")
#define OP2(op) O2(#op, "%0, ", #op); O2(#op "s", "%0, ", #op "s")
#define OPC(op) O2(#op, "%2, ", #op)
/* a PC-relative operand: the value read as pc is the address + 8 (+12 with a register shift) */
#define PCOP(name, insn) do { u32 r = 0; asm volatile(insn : "=r"(r)); out(name, 0, 0, r, 0); } while (0)

static u32 mem[256];
static void memclear(void) { for (int i = 0; i < 256; ++i) mem[i] = 0x01010101u * (u32)(i + 1) ^ 0xA5C3E1F0u; }
static u32 memhash(void) { u32 h = 0; for (int i = 0; i < 256; ++i) h = (h * 31) ^ mem[i]; return h; }

/* load/store with one addressing form: %0 = loaded value / stored value, %1 = base (updated by write-back), %2 = index register */
#define LS(name, insn, ld) do { for (int i = 0; i < N; ++i) { memclear(); u32 v = rnd(), idx = (rnd() & 7) * 8, base = (u32)&mem[128], r = ld ? 0x77 : v; \
	asm volatile(insn : "+r"(r), "+r"(base) : "r"(idx) : "memory"); out(name, v, idx, r, (base - (u32)&mem[128]) ^ memhash()); } } while (0)
#define LSALL(op, sz, shift, ld) \
	LS(op "o8", op " %0, [%1, #8]", ld); LS(op "o-8", op " %0, [%1, #-8]", ld); LS(op "pre", op " %0, [%1, #12]!", ld); LS(op "prem", op " %0, [%1, #-12]!", ld); \
	LS(op "post", op " %0, [%1], #16", ld); LS(op "postm", op " %0, [%1], #-16", ld); LS(op "r", op " %0, [%1, %2]", ld); LS(op "rm", op " %0, [%1, -%2]", ld); \
	LS(op "rpre", op " %0, [%1, %2]!", ld); LS(op "rpost", op " %0, [%1], %2", ld)
#define LSSH(op, ld) \
	LS(op "lsl2", op " %0, [%1, %2, lsl #2]", ld); LS(op "lsl2m", op " %0, [%1, -%2, lsl #2]", ld); LS(op "lsl2pre", op " %0, [%1, %2, lsl #2]!", ld); \
	LS(op "lsl3post", op " %0, [%1], %2, lsl #3", ld); LS(op "lsr1", op " %0, [%1, %2, lsr #1]", ld); LS(op "asr1", op " %0, [%1, %2, asr #1]", ld); \
	LS(op "ror1", op " %0, [%1, %2, ror #1]", ld)

/* LDM/STM: r4-r8 are loaded from / stored to memory at mem[48 + ...]; the base register is %1 */
#define LDMT(name, insn) do { for (int i = 0; i < 3; ++i) { memclear(); u32 base = (u32)&mem[128], x4, x5, x6, x7, x8; \
	asm volatile(insn "\n\tmov %1, r4\n\tmov %2, r5\n\tmov %3, r6\n\tmov %4, r7\n\tmov %5, r8" : "+r"(base), "=r"(x4), "=r"(x5), "=r"(x6), "=r"(x7), "=r"(x8) :: "r4", "r5", "r6", "r7", "r8", "memory"); \
	out(name, x4 ^ x8, x5 ^ x7, x6, (base - (u32)&mem[128]) ^ memhash() ^ (u32)i); } } while (0)
#define STMT(name, insn) do { for (int i = 0; i < 3; ++i) { memclear(); u32 base = (u32)&mem[128], a = rnd(), b = rnd(), c = rnd(), d = rnd(), e = rnd(); \
	asm volatile("mov r4, %2\n\tmov r5, %3\n\tmov r6, %4\n\tmov r7, %5\n\tmov r8, %6\n\t" insn : "+r"(base) : "r"(0), "r"(a), "r"(b), "r"(c), "r"(d), "r"(e) : "r4", "r5", "r6", "r7", "r8", "memory"); \
	out(name, a ^ e, b ^ d, c, (base - (u32)&mem[128]) ^ memhash()); } } while (0)
/* note: in LDMT/STMT %0 = base; %1.. are the other operands (the instruction text names r4-r8 and %0) */

extern u32 thumb_inc(u32);
asm(".text\n.syntax unified\n.thumb\n.thumb_func\n.global thumb_inc\nthumb_inc:\n\tadds r0, #1\n\tlsls r0, r0, #1\n\tbx lr\n.arm\n");

void _start(void)
{
	/* data processing: dest = op(Rn, operand2), flags in and out */
	OP3(and); OP3(eor); OP3(sub); OP3(rsb); OP3(add); OP3(adc); OP3(sbc); OP3(rsc); OP3(orr); OP3(bic);
	OP2(mov); OP2(mvn);
	OPC(tst); OPC(teq); OPC(cmp); OPC(cmn);

	/* pc as an operand */
	PCOP("movpc", "mov %0, pc"); PCOP("addpc", "add %0, pc, #4"); PCOP("subpc", "sub %0, pc, #8");
	PCOP("ldrlit", "ldr %0, 1f\n\tb 2f\n1: .word 0x13572468\n2:");
	PCOP("ldrpcrel", "adr %0, 3f\n\tldr %0, [%0, #-4]\n\tb 4f\n3: .word 0\n4:");
	PCOP("rsbpc", "rsb %0, pc, #0x100"); PCOP("movlsl", "mov %0, pc, lsl #1");

	/* loads and stores */
	LSALL("ldr", 4, 2, 1); LSALL("str", 4, 2, 0); LSALL("ldrb", 1, 0, 1); LSALL("strb", 1, 0, 0);
	LSSH("ldr", 1); LSSH("str", 0); LSSH("ldrb", 1); LSSH("strb", 0);
	LS("ldrh", "ldrh %0, [%1, #6]", 1); LS("ldrhm", "ldrh %0, [%1, #-6]", 1); LS("ldrhr", "ldrh %0, [%1, %2]", 1); LS("ldrhpost", "ldrh %0, [%1], #10", 1);
	LS("ldrhpre", "ldrh %0, [%1, #-10]!", 1); LS("ldrsb", "ldrsb %0, [%1, #5]", 1); LS("ldrsbr", "ldrsb %0, [%1, -%2]", 1); LS("ldrsh", "ldrsh %0, [%1, #6]", 1);
	LS("ldrshpost", "ldrsh %0, [%1], #-4", 1);
	LS("strh", "strh %0, [%1, #6]", 0); LS("strhm", "strh %0, [%1, #-6]", 0); LS("strhr", "strh %0, [%1, %2]", 0); LS("strhpost", "strh %0, [%1], #10", 0);
	LS("strhpre", "strh %0, [%1, #-10]!", 0);

	/* LDM / STM */
	LDMT("ldmia", "ldmia %0, {r4, r5, r7}"); LDMT("ldmiaw", "ldmia %0!, {r4, r6, r8}"); LDMT("ldmib", "ldmib %0, {r4, r5, r6, r7, r8}");
	LDMT("ldmibw", "ldmib %0!, {r5, r7}"); LDMT("ldmda", "ldmda %0, {r4, r6}"); LDMT("ldmdaw", "ldmda %0!, {r4, r5, r6, r7}");
	LDMT("ldmdb", "ldmdb %0, {r4, r8}"); LDMT("ldmdbw", "ldmdb %0!, {r4, r5, r6, r7, r8}");
	STMT("stmia", "stmia %0, {r4, r5, r7}"); STMT("stmiaw", "stmia %0!, {r4, r6, r8}"); STMT("stmib", "stmib %0, {r4, r5, r6, r7, r8}");
	STMT("stmibw", "stmib %0!, {r5, r7}"); STMT("stmda", "stmda %0, {r4, r6}"); STMT("stmdaw", "stmda %0!, {r4, r5, r6, r7}");
	STMT("stmdb", "stmdb %0, {r4, r8}"); STMT("stmdbw", "stmdb %0!, {r4, r5, r6, r7, r8}");
	{ u32 r; memclear(); asm volatile("mov r4, #5\n\tmov r5, #9\n\tpush {r4, r5}\n\tpop {r6, r7}\n\tadd %0, r6, r7" : "=r"(r) :: "r4", "r5", "r6", "r7", "memory"); out("pushpop", 0, 0, r, 0); }

	/* every condition over every NZCV */
	#define CC(c) do { for (u32 f = 0; f < 16; ++f) { u32 r = 0, fl = f << 28; \
		asm volatile("msr APSR_nzcvq, %1\n\tmov %0, #0\n\tmov" #c " %0, #1" : "+r"(r) : "r"(fl) : "cc"); out("cond-" #c, f, 0, r, 0); } } while (0)
	CC(eq); CC(ne); CC(cs); CC(cc); CC(mi); CC(pl); CC(vs); CC(vc); CC(hi); CC(ls); CC(ge); CC(lt); CC(gt); CC(le); CC();
	#define CB(c) do { for (u32 f = 0; f < 16; ++f) { u32 r = 0, fl = f << 28; \
		asm volatile("msr APSR_nzcvq, %1\n\tmov %0, #1\n\tb" #c " 1f\n\tmov %0, #2\n1:" : "+r"(r) : "r"(fl) : "cc"); out("bcond-" #c, f, 0, r, 0); } } while (0)
	CB(eq); CB(ne); CB(cs); CB(cc); CB(mi); CB(pl); CB(vs); CB(vc); CB(hi); CB(ls); CB(ge); CB(lt); CB(gt); CB(le);

	/* multiplies */
	#define MU(name, insn) do { for (int i = 0; i < N * 2; ++i) { u32 a = rnd(), b = rnd(), c = rnd(), lo = c, hi = rnd(), r = 0, fl = rnd() & 0xF0000000u, fo; \
		asm volatile("msr APSR_nzcvq, %6\n\t" insn "\n\tmrs %3, APSR" : "+r"(lo), "+r"(hi), "+r"(r), "=&r"(fo) : "r"(a), "r"(b), "r"(fl) : "cc"); \
		out(name, a ^ b, lo, hi ^ r, fo & 0xF0000000u); } } while (0)
	MU("mul", "mul %2, %4, %5"); MU("muls", "muls %2, %4, %5"); MU("mla", "mla %2, %4, %5, %0"); MU("mlas", "mlas %2, %4, %5, %0");
	MU("umull", "umull %0, %1, %4, %5"); MU("umulls", "umulls %0, %1, %4, %5"); MU("smull", "smull %0, %1, %4, %5"); MU("smulls", "smulls %0, %1, %4, %5");
	MU("umlal", "umlal %0, %1, %4, %5"); MU("umlals", "umlals %0, %1, %4, %5"); MU("smlal", "smlal %0, %1, %4, %5"); MU("smlals", "smlals %0, %1, %4, %5");

	/* branches, links, interworking */
	{ u32 r; asm volatile("bl 1f\n1:\tmov %0, lr" : "=r"(r) :: "lr"); out("bl", 0, 0, r, 0); }
	{ u32 r; asm volatile("adr r3, 1f\n\tblx r3\n1:\tmov %0, lr" : "=r"(r) :: "lr", "r3"); out("blxr", 0, 0, r, 0); }
	{ u32 r; asm volatile("adr r3, 1f\n\tbx r3\n\tmov %0, #7\n\tb 2f\n1:\tmov %0, #9\n2:" : "=r"(r) :: "r3"); out("bxr", 0, 0, r, 0); }
	for (u32 i = 0; i < 6; ++i) out("thumb", i, 0, thumb_inc(i * 0x11111111u), 0);
	{ u32 r; asm volatile("adr r3, 1f\n\tmov lr, pc\n\tbx r3\n\tb 2f\n1:\tmov %0, lr\n\tbx lr\n2:" : "=r"(r) :: "lr", "r3"); out("bxlr", 0, 0, r, 0); }
	{ u32 r = 3; asm volatile("adr lr, 1f\n\tmov pc, lr\n\tmov %0, #1\n1:\tadd %0, %0, #2" : "+r"(r) :: "lr"); out("movpclr", 0, 0, r, 0); }
	{ u32 r = 3; asm volatile("adr r3, 1f\n\tldr pc, [r3]\n\tmov %0, #1\n\tb 2f\n1:\t.word 3f\n3:\tadd %0, %0, #5\n2:" : "+r"(r) :: "r3"); out("ldrpc", 0, 0, r, 0); }
	{ u32 r = 0; asm volatile("adr r3, 1f\n\tadd pc, r3, #0\n\tmov %0, #1\n1:\tadd %0, %0, #8" : "+r"(r) :: "r3"); out("addpc", 0, 0, r, 0); }

	/* exclusives, MOVW / MOVT */
	for (int i = 0; i < 4; ++i)
	{
		u32 v = rnd(), w = rnd(), r, s1, s2, s3;
		memclear();
		asm volatile("ldrex %0, [%4]\n\tadd %1, %0, %5\n\tstrex %2, %1, [%4]\n\tclrex\n\tstrex %3, %5, [%4]" : "=&r"(r), "=&r"(s1), "=&r"(s2), "=&r"(s3) : "r"(&mem[7]), "r"(v) : "memory");
		out("ldrex", v, r, s2 | (s3 << 4), memhash() ^ s1);
		asm volatile("ldrexb %0, [%2]\n\tstrexb %1, %3, [%2]" : "=&r"(r), "=&r"(s2) : "r"(&mem[9]), "r"(w) : "memory");
		out("ldrexb", w, r, s2, memhash());
		asm volatile("ldrexh %0, [%2]\n\tstrexh %1, %3, [%2]" : "=&r"(r), "=&r"(s2) : "r"(&mem[11]), "r"(w) : "memory");
		out("ldrexh", w, r, s2, memhash());
	}
	{ u32 r; asm volatile("movw %0, #0x1234\n\tmovt %0, #0xabcd" : "=r"(r)); out("movwt", 0, 0, r, 0); }
	sys_exit(0);
}

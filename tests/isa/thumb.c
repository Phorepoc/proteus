// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
/* thumb.c -- the Proteus Thumb-1 differential test (run.sh: qemu-arm vs armcpu.cpp). Every Thumb format (the fast loop in armfast.inc and
 * ThumbStep in thumb.cpp implement the same ISA) runs on random inputs with random flags going in; the result registers, the NZCV flags (captured
 * with flag-free hi-register adds and branches, as ARMv5 Thumb has no MRS) and a hash of memory come out. Each snippet is entered from ARM state by
 * BX and left by BX, so the interworking is covered too. Build: -march=armv5te (the assembler then rejects Thumb-2 encodings). */
typedef unsigned int u32;

static void sys_write(const char* p, u32 n)
{
	register u32 r0 asm("r0") = 1, r1 asm("r1") = (u32)p, r2 asm("r2") = n, r7 asm("r7") = 4;
	asm volatile("swi #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
static void sys_exit(u32 code)
{
	register u32 r0 asm("r0") = code, r7 asm("r7") = 1;
	asm volatile("swi #0" : : "r"(r0), "r"(r7));
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
static u32 seed = 0x13579bdf;
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
static u32 mem[128];
static void memclear(void) { for (int i = 0; i < 128; ++i) mem[i] = 0x01010101u * (u32)(i + 1) ^ 0xA5C3E1F0u; }
static u32 memhash(void) { u32 h = 0; for (int i = 0; i < 128; ++i) h = (h * 31) ^ mem[i]; return h; }

#define N 5
/* body = Thumb-1 code on r0-r3 (r3 = &mem[64] on entry). Flags: msr before, captured after (N = 8, Z = 4, C = 2, V = 1). */
#define TH(name, body) do { for (int i = 0; i < N; ++i) { \
	u32 a = rnd(), b = rnd(), c = shamt(), f = rnd() & 0xF0000000u, fo, o0, o1, o2; memclear(); \
	register u32 r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c, r3 asm("r3") = (u32)&mem[64]; \
	asm volatile( \
		"msr CPSR_f, %[f]\n\t" \
		"mov r8, #0\n\tmov r9, #8\n\tmov r10, #4\n\tmov r11, #2\n\tmov r12, #1\n\t" \
		"adr r4, 1f\n\torr r4, r4, #1\n\tbx r4\n\t" \
		".balign 4\n\t.thumb\n\t.syntax unified\n1:\n\t" \
		body "\n\t" \
		"bpl 11f\n\tadd r8, r9\n11:\n\tbne 12f\n\tadd r8, r10\n12:\n\tbcc 13f\n\tadd r8, r11\n13:\n\tbvc 14f\n\tadd r8, r12\n14:\n\t" \
		"adr r4, 15f\n\tbx r4\n\t.balign 4\n\t.arm\n15:\n\tmov %[fo], r8\n\t" \
		: "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), [fo] "=r"(fo) : [f] "r"(f) : "r4", "r8", "r9", "r10", "r11", "r12", "lr", "cc", "memory"); \
	o0 = r0; o1 = r1; o2 = r2; \
	out(name, a ^ (c << 3), b, o0 ^ (o1 * 3) ^ (o2 * 5), (fo << 28) ^ (r3 - (u32)&mem[64]) ^ memhash() ^ (f >> 4)); } } while (0)

void _start(void)
{
	/* format 1: shift by immediate */
	TH("lsl0", "lsls r0, r1, #0"); TH("lsl5", "lsls r0, r1, #5"); TH("lsl31", "lsls r0, r1, #31");
	TH("lsr0", "lsrs r0, r1, #32"); TH("lsr7", "lsrs r0, r1, #7"); TH("asr0", "asrs r0, r1, #32"); TH("asr9", "asrs r0, r1, #9");
	/* format 2: add / subtract */
	TH("addr", "adds r0, r0, r1"); TH("subr", "subs r0, r0, r1"); TH("addi3", "adds r0, r1, #3"); TH("subi3", "subs r0, r1, #5");
	/* format 3: immediate */
	TH("movi", "movs r0, #200"); TH("cmpi", "cmp r0, #77"); TH("addi", "adds r0, #99"); TH("subi", "subs r0, #255");
	/* format 4: ALU */
	TH("ands", "ands r0, r1"); TH("eors", "eors r0, r1"); TH("lslr", "lsls r0, r2"); TH("lsrr", "lsrs r0, r2"); TH("asrr", "asrs r0, r2");
	TH("adcs", "adcs r0, r1"); TH("sbcs", "sbcs r0, r1"); TH("rors", "rors r0, r2"); TH("tst", "tst r0, r1"); TH("negs", "negs r0, r1");
	TH("cmpr", "cmp r0, r1"); TH("cmn", "cmn r0, r1"); TH("orrs", "orrs r0, r1"); TH("muls", "muls r0, r1"); TH("bics", "bics r0, r1"); TH("mvns", "mvns r0, r1");
	/* format 5: hi registers (no flags except CMP) */
	TH("hiadd", "mov r8, r1\n\tadd r0, r8"); TH("hicmp", "mov r8, r1\n\tcmp r0, r8"); TH("himov", "mov r8, r0\n\tmov r1, r8"); TH("hiaddhh", "mov r8, r0\n\tmov r9, r1\n\tadd r8, r9\n\tmov r0, r8");
	TH("hipc", "mov r1, pc"); TH("hiaddpc", "add r1, pc"); TH("hilr", "mov r1, lr");
	TH("bxarm", "adr r1, 5f\n\tbx r1\n\t.balign 4\n\t.arm\n5:\tadd r0, r0, #1\n\tadr r1, 6f\n\torr r1, r1, #1\n\tbx r1\n\t.balign 2\n\t.thumb\n6:");
	TH("blx", "adr r1, 5f\n\tblx r1\n\tb 7f\n\t.balign 4\n\t.arm\n5:\tadd r0, r0, #2\n\tbx lr\n\t.thumb\n7:\n\tmov r1, lr");
	/* format 6: PC-relative load */
	TH("ldrpc", "ldr r0, 5f\n\tb 6f\n\t.balign 4\n5:\t.word 0x13572468\n6:");
	/* formats 7/8: register offset (r1 reduced to a small even index) */
	#define RO(n, i) TH(n, "movs r2, #12\n\tands r1, r2\n\t" i)
	RO("strr", "str r0, [r3, r1]"); RO("strhr", "strh r0, [r3, r1]"); RO("strbr", "strb r0, [r3, r1]"); RO("ldrsbr", "ldrsb r0, [r3, r1]");
	RO("ldrr", "ldr r0, [r3, r1]"); RO("ldrhr", "ldrh r0, [r3, r1]"); RO("ldrbr", "ldrb r0, [r3, r1]"); RO("ldrshr", "ldrsh r0, [r3, r1]");
	/* formats 9/10/11 */
	TH("stri", "str r0, [r3, #12]"); TH("ldri", "ldr r0, [r3, #16]"); TH("strbi", "strb r0, [r3, #5]"); TH("ldrbi", "ldrb r0, [r3, #7]");
	TH("strhi", "strh r0, [r3, #6]"); TH("ldrhi", "ldrh r0, [r3, #10]");
	TH("spls", "sub sp, #16\n\tstr r0, [sp, #8]\n\tldr r1, [sp, #8]\n\tadd sp, #16");
	/* formats 12/13 */
	TH("addsp", "add r0, sp, #0\n\tsub sp, #8\n\tadd r1, sp, #4\n\tadd sp, #8\n\tsubs r1, r1, r0\n\tmovs r0, #0"); TH("addpcr", "add r1, pc, #8");
	/* format 14: push / pop */
	TH("pushpop", "push {r0, r1}\n\tpop {r2, r3}");
	TH("pushlr", "push {r0, lr}\n\tpop {r1, r2}");
	/* format 15: STMIA / LDMIA */
	TH("stmia", "stmia r3!, {r0, r1}"); TH("ldmia", "ldmia r3!, {r0, r2}");
	/* format 16/18: conditional and unconditional branches */
	#define BC(c) TH("b" #c, "b" #c " 1f\n\tmovs r0, #1\n\tb 2f\n1:\tmovs r0, #2\n2:")
	BC(eq); BC(ne); BC(cs); BC(cc); BC(mi); BC(pl); BC(vs); BC(vc); BC(hi); BC(ls); BC(ge); BC(lt); BC(gt); BC(le);
	TH("bback", "b 2f\n1:\tmovs r0, #3\n\tb 3f\n2:\tb 1b\n3:");
	/* format 19: BL */
	TH("bl", "bl 1f\n1:\tmov r1, lr"); TH("blback", "b 2f\n1:\tmov r1, lr\n\tbx lr\n2:\tbl 1b\n\tmov r0, lr");
	sys_exit(0);
}

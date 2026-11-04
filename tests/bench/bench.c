/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0
 * bench.c -- a freestanding ARM guest that exercises the code paths a Qt/Symbian start-up spends its instructions in (measured pc samples of the
 * Sparkle load: euser atomic LDREX/STREX loops, RFastLock, memset/memcpy, RHeap free-list walks, soft-float (libgcc dfpaeabi), qtcore
 * reference counting, virtual calls, string compares). Run by bench.cpp on the interpreter; the printed checksum must not change with the interpreter. */
typedef unsigned int u32; typedef unsigned char u8;
static inline void sys_exit(u32 c) { register u32 r0 asm("r0") = c; register u32 r7 asm("r7") = 1; asm volatile("svc 0" :: "r"(r0), "r"(r7)); for (;;); }
static inline void sys_write(const char* s, u32 n) { register u32 r0 asm("r0") = 1; register const char* r1 asm("r1") = s; register u32 r2 asm("r2") = n; register u32 r7 asm("r7") = 4; asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory"); }

#ifdef BENCH_THUMB
/* Thumb-1 (ARMv5T): no LDREX/STREX, no CLZ; plain read-modify-write stands in for the atomics */
static u32 __attribute__((noinline)) atomic_add(volatile u32* p, u32 v) { u32 o = *p; *p = o + v; return o; }
static u32 __attribute__((noinline)) atomic_cas(volatile u32* p, u32 expect, u32 nv) { u32 o = *p; if (o == expect) *p = nv; return o; }
static int clz32(u32 x) { int n = 0; while (!(x & 0x80000000u)) { x <<= 1; ++n; } return n; }
#else
static int clz32(u32 x) { return __builtin_clz(x); }
static u32 __attribute__((noinline)) atomic_add(volatile u32* p, u32 v)
{
	u32 old, tmp, st;
	asm volatile("1: ldrex %0, [%3]\n add %1, %0, %4\n strex %2, %1, [%3]\n cmp %2, #0\n bne 1b\n" : "=&r"(old), "=&r"(tmp), "=&r"(st) : "r"(p), "r"(v) : "cc", "memory");
	return old;
}
static u32 __attribute__((noinline)) atomic_cas(volatile u32* p, u32 expect, u32 nv)
{
	u32 old, st;
	asm volatile("1: ldrex %0, [%2]\n cmp %0, %3\n bne 2f\n strex %1, %4, [%2]\n cmp %1, #0\n bne 1b\n 2:\n" : "=&r"(old), "=&r"(st) : "r"(p), "r"(expect), "r"(nv) : "cc", "memory");
	return old;
}
#endif
void* memset(void* d, int c, u32 n) { u8* p = d; while (n--) *p++ = (u8)c; return d; }
void* memcpy(void* d, const void* s, u32 n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }
static void __attribute__((noinline)) wordset(u32* p, u32 v, u32 n) { u32 i; for (i = 0; i + 4 <= n; i += 4) { p[i] = v; p[i+1] = v; p[i+2] = v; p[i+3] = v; } }

struct node { struct node* next; u32 size; u32 key; };
struct obj { const struct vt* v; u32 rc; u32 val; };
struct vt { u32 (*f)(struct obj*, u32); u32 (*g)(struct obj*, u32); };
static u32 vf(struct obj* o, u32 x) { return o->val + x; }
static u32 vg(struct obj* o, u32 x) { return atomic_add(&o->rc, 1) + (x ^ o->val); }
static const struct vt VT = { vf, vg };

static u8 heap[1 << 16]; static u32 buf[8192]; static struct node nodes[512]; static struct obj objs[64];
/* a small software floating point (what dfpaeabi does in the real euser): sign/exponent/32-bit mantissa, normalisation by CLZ, UMULL */
typedef struct { u32 s; int e; u32 m; } sf;
static sf __attribute__((noinline)) sf_norm(sf a) { if (a.m) { int n = clz32(a.m); a.m <<= n; a.e -= n; } return a; }
static sf __attribute__((noinline)) sf_mul(sf a, sf b) { sf r; unsigned long long p = (unsigned long long)a.m * b.m; r.s = a.s ^ b.s; r.e = a.e + b.e + 32; r.m = (u32)(p >> 32); return sf_norm(r); }
static sf __attribute__((noinline)) sf_add(sf a, sf b)
{
	sf r; int d;
	if (a.e < b.e) { sf t = a; a = b; b = t; }
	d = a.e - b.e; if (d > 31) return a;
	r.e = a.e + 1; r.s = a.s;
	{ unsigned long long x = (unsigned long long)a.m, y = (unsigned long long)b.m >> d; unsigned long long z = (a.s == b.s) ? x + y : x - y; r.m = (u32)(z >> 1); if (a.s != b.s) { r.m = (u32)z; r.e = a.e; } }
	return sf_norm(r);
}
static int __attribute__((noinline)) scmp(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }

int main(void)
{
	u32 sum = 0, i, r; sf dacc = { 0, 0, 0x80000000u }, facc = { 0, 0, 0xC0000000u }, k1 = { 0, 0, 0x80010000u }, k2 = { 0, -2, 0x80000000u };
	static const char* names[] = { "QObject", "QWidget", "QGraphicsView", "QApplication", "QEventDispatcher", "QString" };
	for (i = 0; i < 512; ++i) { nodes[i].next = &nodes[(i * 37 + 11) & 511]; nodes[i].size = (i * 13) & 255; nodes[i].key = i; }
	for (i = 0; i < 64; ++i) { objs[i].v = &VT; objs[i].rc = 1; objs[i].val = i * 7; }
	for (r = 0; r < 300; ++r)
	{
		for (i = 0; i < 40; ++i) sum += atomic_add(&buf[i & 15], i);			/* Qt ref counts */
		for (i = 0; i < 40; ++i) sum += atomic_cas(&buf[16 + (i & 7)], buf[16 + (i & 7)], i + r);
		memset(heap, (int)r, 2048);											/* RHeap / memset */
		memcpy(heap + 4096, heap, 1024);
		wordset(buf, r, 8192);
		{ struct node* n = &nodes[r & 511]; for (i = 0; i < 400; ++i) { sum += n->size + n->key; n = n->next; } }	/* free-list / list walk */
		for (i = 0; i < 100; ++i) { struct obj* o = &objs[(i * 5 + r) & 63]; sum += o->v->f(o, i) + o->v->g(o, r); }	/* virtual calls */
		for (i = 0; i < 30; ++i) sum += scmp(names[i % 6], names[(i + r) % 6]) & 0xFF;
		for (i = 0; i < 20; ++i) { dacc = sf_add(sf_mul(dacc, k1), k2); facc = sf_add(sf_mul(facc, k1), k2); if (dacc.e > 64) dacc.e = 0; if (facc.e > 64) facc.e = 0; }		/* soft float (libgcc) */
		for (i = 0; i < 200; ++i) { u32 x = i ^ r; sum += (x & 1) ? (x >> 3) : (x << 2); sum ^= (sum << 5) | (sum >> 27); sum += (x * 2654435761u) >> 7; } /* ALU mix */
	}
	sum += dacc.m + facc.m + (u32)dacc.e;
	{ char out[16]; int k; for (k = 0; k < 8; ++k) out[k] = "0123456789abcdef"[(sum >> (28 - 4 * k)) & 15]; out[8] = '\n'; sys_write(out, 9); }
	sys_exit(0);
	return 0;
}

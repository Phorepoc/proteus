// e32decomp.cpp -- OURS (Proteus P2): the two E32Image compressions, pure C++ (host test + x32 proteus.exe).
// Byte-pair (0x102822AA): [index: i32 size, i32 usize, u16 pages, u16 page sizes][pages]; a page = pair table + token stream (emitter/bytepair.py).
// Symbian deflate (0x101F7AFC, NOT zlib): port of emitter/e32deflate.py = e32tools/e32image/deflate/{decode,inflate}.cpp.
#include "e32decomp.h"
#include <stdlib.h>
#include <string.h>

namespace proteus
{
static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ---- byte-pair ---------------------------------------------------------------------------------------------------------------------------------
struct TBpOut { uint8_t* iP; uint32_t iN, iMax; };

static bool BpExpand(const uint8_t* first, const uint8_t* second, uint8_t tok, TBpOut& o)
{
	uint8_t stack[512];
	int sp = 0;
	stack[sp++] = tok;
	while (sp)
	{
		const uint8_t b = stack[--sp];
		if (first[b] == b)
		{
			if (o.iN >= o.iMax) return true;		// page is full: the rest is padding
			o.iP[o.iN++] = b;
			continue;
		}
		if (sp > 509) return false;
		stack[sp++] = second[b];
		stack[sp++] = first[b];
	}
	return true;
}

static bool BpPage(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t max, uint32_t& got)
{
	uint8_t first[256], second[256];
	for (int i = 0; i < 256; ++i) { first[i] = (uint8_t)i; second[i] = 0; }
	uint32_t p = 0;
	if (!len) return false;
	const uint32_t pairs = in[p++];
	int marker = -1;
	if (pairs)
	{
		if (p >= len) return false;
		marker = in[p++];
		first[marker] = (uint8_t)~marker;
		if (pairs < 32)
		{
			if (p + 3 * pairs > len) return false;
			for (uint32_t i = 0; i < pairs; ++i, p += 3) { first[in[p]] = in[p + 1]; second[in[p]] = in[p + 2]; }
		}
		else
		{
			if (p + 32 > len) return false;
			const uint8_t* mask = in + p;
			p += 32;
			uint32_t left = pairs;
			for (int b = 0; b < 256; ++b)
				if (mask[b >> 3] & (1 << (b & 7)))
				{
					if (p + 2 > len) return false;
					first[b] = in[p]; second[b] = in[p + 1]; p += 2; --left;
				}
			if (left) return false;
		}
	}
	TBpOut o = { out, 0, max };
	while (p < len && o.iN < max)
	{
		const uint8_t b = in[p++];
		if (b == marker)
		{
			if (p >= len) return false;
			out[o.iN++] = in[p++];
			continue;
		}
		if (!BpExpand(first, second, b, o)) return false;
	}
	got = o.iN;
	return true;
}

// one section at in[0..]: returns bytes consumed (0 = error), writes exactly aSize bytes
static uint32_t BpSection(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t size)
{
	if (len < 10) return 0;
	const uint32_t pages = U16(in + 8);
	uint32_t p = 10 + 2 * pages;
	if (p > len) return 0;
	uint32_t done = 0;
	for (uint32_t i = 0; i < pages; ++i)
	{
		const uint32_t ps = U16(in + 10 + 2 * i);
		if (p + ps > len) return 0;
		const uint32_t want = size - done < 4096 ? size - done : 4096;
		uint32_t got = 0;
		if (!BpPage(in + p, ps, out + done, want, got)) return 0;
		done += got;
		p += ps;
	}
	return done == size ? p : 0;
}

// ---- Symbian deflate ---------------------------------------------------------------------------------------------------------------------------
enum { KMinLength = 3, KDistCodeBase = 0x200, ELiterals = 256, ELengths = 28, ELitLens = ELiterals + ELengths + 1, EEos = ELiterals + ELengths,
	EDistances = 44, KDeflationCodes = ELitLens + EDistances, KMaxCodeLength = 27, KMetaCodes = 28 };

static const uint32_t KHuffmanDecoding[] =
{
	0x0004006c, 0x00040064, 0x0004005c, 0x00040050, 0x00040044, 0x0004003c, 0x00040034, 0x00040021, 0x00040023, 0x00040025, 0x00040027, 0x00040029,
	0x00040014, 0x0004000c, 0x00040035, 0x00390037, 0x00330031, 0x0004002b, 0x002f002d, 0x001f001d, 0x001b0019, 0x00040013, 0x00170015, 0x0004000d,
	0x0011000f, 0x000b0009, 0x00070003, 0x00050001
};

struct TBits
{
	const uint8_t* iD;
	uint32_t iLen, iPos;			// bit position
	bool iBad;
	uint32_t Bit()
	{
		if ((iPos >> 3) >= iLen) { iBad = true; return 0; }
		const uint32_t p = iPos++;
		return (iD[p >> 3] >> (7 - (p & 7))) & 1;
	}
	uint32_t Read(int n) { uint32_t v = 0; while (n--) v = (v << 1) | Bit(); return v; }
	uint32_t Huffman(const uint32_t* t)
	{
		uint32_t idx = 0, huff = 0;
		for (int guard = 0; guard < 64 && !iBad; ++guard)
		{
			idx += (huff >> 16) / 4;
			huff = t[idx];
			if (Bit() == 0) huff <<= 16;
			if (huff & 0x10000) return huff >> 17;
		}
		iBad = true;
		return 0;
	}
};

static int SubTree(uint32_t* t, int ptr, int value, const int* levels, int li)
{
	const int l = levels[li++];
	if (l > value)
	{
		const int sub0 = SubTree(t, ptr, value, levels, li);
		ptr = SubTree(t, sub0, value - (ptr - sub0) - 1, levels, li);
		const uint32_t branch0 = (uint32_t)(sub0 - (ptr - 1)) * 4;
		--ptr;
		t[ptr] = (4u << 16) | branch0;
	}
	else if (l == value)
	{
		const uint32_t term0 = t[value--];
		ptr = SubTree(t, ptr, value, levels, li);
		--ptr;
		t[ptr] = (4u << 16) | (term0 >> 16);
	}
	else
	{
		const uint32_t term0 = t[value--];
		const uint32_t term1 = t[value--];
		--ptr;
		t[ptr] = ((term1 >> 16) << 16) | (term0 >> 16);
	}
	return ptr;
}

static void HuffmanDecoding(const int* lengths, int n, uint32_t* t, uint32_t symbolBase)
{
	int counts[KMaxCodeLength] = { 0 };
	int codes = 0;
	for (int i = 0; i < n; ++i)
	{
		t[i] = (uint32_t)lengths[i];
		if (lengths[i] >= 1) { ++counts[lengths[i] - 1]; ++codes; }
	}
	int levels[KMaxCodeLength];
	int lit = codes;
	for (int i = 0; i < KMaxCodeLength; ++i) { levels[i] = lit; lit -= counts[i]; }
	const uint32_t base = (symbolBase << 17) + (1u << 16);
	for (int i = 0; i < n; ++i)
	{
		const int ln = (int)(t[i] & 0xFF);
		if (ln) { --levels[ln - 1]; t[levels[ln - 1]] |= ((uint32_t)i << 17) + base; }
	}
	if (codes == 1) { const uint32_t term = t[0] >> 16; t[0] = term | (term << 16); }
	else if (codes > 1) SubTree(t, codes - 1, codes - 1, levels, 0);
}

static bool Valid(const int* l, int n)
{
	uint64_t s = 0;
	int cnt = 0;
	for (int i = 0; i < n; ++i) if (l[i]) { if (l[i] > KMaxCodeLength) return false; s += 1ull << (KMaxCodeLength - l[i]); ++cnt; }
	return cnt <= 1 || s == (1ull << KMaxCodeLength);
}

static bool Internalize(TBits& bits, int* out, int num)
{
	int lst[KMetaCodes];
	for (int i = 0; i < KMetaCodes; ++i) lst[i] = i;
	int last = 0, n = 0, rl = 0;
	while (n + rl < num && !bits.iBad)
	{
		int c = (int)bits.Huffman(KHuffmanDecoding);
		if (c < 2) rl += rl + c + 1;
		else
		{
			if (n + rl >= num) return false;
			while (rl) { out[n++] = last; --rl; }
			--c;
			if (c >= KMetaCodes) return false;
			lst[0] = last;
			last = lst[c];
			memmove(lst + 1, lst, c * sizeof(int));
			out[n++] = last;
		}
	}
	if (n + rl > num) return false;
	while (rl) { out[n++] = last; --rl; }
	return !bits.iBad;
}

static bool Inflate(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t size)
{
	TBits bits = { in, len, 0, false };
	int lens[KDeflationCodes];
	if (!Internalize(bits, lens, KDeflationCodes)) return false;
	if (!Valid(lens, ELitLens) || !Valid(lens + ELitLens, EDistances)) return false;
	static uint32_t lt[ELitLens], dt[EDistances];
	HuffmanDecoding(lens, ELitLens, lt, 0);
	HuffmanDecoding(lens + ELitLens, EDistances, dt, KDistCodeBase);
	const uint32_t* tree = lt;
	uint32_t n = 0, ln = 0;
	while (n < size && !bits.iBad)
	{
		const int val = (int)bits.Huffman(tree) - ELiterals;
		if (val < 0) { out[n++] = (uint8_t)val; continue; }
		if (val == EEos - ELiterals) break;
		uint32_t code = (uint32_t)val & 0xFF;
		if (code >= 8)
		{
			const int xtra = (int)(code >> 2) - 1;
			code -= (uint32_t)xtra << 2;
			code <<= xtra;
			code |= bits.Read(xtra);
		}
		if (val < KDistCodeBase - ELiterals) { ln = code + KMinLength; tree = dt; continue; }
		if (code + 1 > n) return false;
		uint32_t src = n - (code + 1);
		for (uint32_t k = 0; k < ln && n < size; ++k) out[n++] = out[src++];
		tree = lt;
	}
	return n == size && !bits.iBad;
}

uint8_t* DecompressE32(const uint8_t* f, uint32_t len, uint32_t& outLen, const char*& err)
{
	err = "";
	if (len < 128) { err = "truncated header"; return NULL; }
	const uint32_t type = U32(f + 28), codeSize = U32(f + 48), codeOff = U32(f + 100), usize = U32(f + 124);
	if (type != KCompressBytePair && type != KCompressDeflate) { err = "unknown compression type"; return NULL; }
	if (codeOff > len || usize > (64u << 20) || codeSize > usize) { err = "bad compressed header"; return NULL; }
	uint8_t* out = (uint8_t*)malloc(codeOff + usize);
	if (!out) { err = "no memory"; return NULL; }
	memcpy(out, f, codeOff);
	bool ok;
	if (type == KCompressDeflate)
		ok = Inflate(f + codeOff, len - codeOff, out + codeOff, usize);
	else
	{
		const uint32_t used = BpSection(f + codeOff, len - codeOff, out + codeOff, codeSize);
		ok = used && BpSection(f + codeOff + used, len - codeOff - used, out + codeOff + codeSize, usize - codeSize);
	}
	if (!ok) { free(out); err = type == KCompressDeflate ? "corrupt deflate stream" : "corrupt byte-pair stream"; return NULL; }
	memset(out + 28, 0, 4);			// now uncompressed
	memset(out + 124, 0, 4);
	outLen = codeOff + usize;
	return out;
}
}

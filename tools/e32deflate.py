# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""Symbian DEFLATE (KUidCompressionDeflate 0x101F7AFC) decoder -- NOT zlib.

Faithful port of e32tools/e32image/deflate/{decode,inflate}.cpp: MSB-first bit
stream, run-length/MTF-coded code-length table, Symbian's own huffman decoding
tree (word array), 4K history window, literal/length/distance scheme.
"""

K_MIN_LENGTH = 3
K_MAX_DISTANCE = 1 << 12
K_DIST_CODE_BASE = 0x200
E_LITERALS = 256
E_LENGTHS = 28
E_LITLENS = E_LITERALS + E_LENGTHS + 1
E_EOS = E_LITERALS + E_LENGTHS
E_DISTANCES = 44
K_DEFLATION_CODES = E_LITLENS + E_DISTANCES
K_MAX_CODE_LENGTH = 27
K_META_CODES = 28
M32 = 0xFFFFFFFF

HUFFMAN_DECODING = [
    0x0004006c, 0x00040064, 0x0004005c, 0x00040050, 0x00040044, 0x0004003c,
    0x00040034, 0x00040021, 0x00040023, 0x00040025, 0x00040027, 0x00040029,
    0x00040014, 0x0004000c, 0x00040035, 0x00390037, 0x00330031, 0x0004002b,
    0x002f002d, 0x001f001d, 0x001b0019, 0x00040013, 0x00170015, 0x0004000d,
    0x0011000f, 0x000b0009, 0x00070003, 0x00050001]


class DeflateError(Exception):
    pass


class BitInput:
    def __init__(self, data, pos=0):
        self.d = data
        self.bitpos = pos * 8

    def bit(self):
        p = self.bitpos
        if (p >> 3) >= len(self.d):
            raise DeflateError("bit stream exhausted")
        self.bitpos = p + 1
        return (self.d[p >> 3] >> (7 - (p & 7))) & 1

    def read(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def huffman(self, tree):
        idx = 0
        huff = 0
        while True:
            idx += (huff >> 16) // 4          # PtrAdd: byte offset
            huff = tree[idx]
            if self.bit() == 0:
                huff = (huff << 16) & M32
            if huff & 0x10000:
                return huff >> 17


def _subtree(t, ptr, value, levels, li):
    """HuffmanSubTree; ptr/value are word indices, returns (ptr, li)."""
    l = levels[li]
    li += 1
    if l > value:
        sub0, _ = _subtree(t, ptr, value, levels, li)
        ptr, _ = _subtree(t, sub0, value - (ptr - sub0) - 1, levels, li)
        branch0 = (sub0 - (ptr - 1)) * 4
        ptr -= 1
        t[ptr] = (4 << 16) | branch0
    elif l == value:
        term0 = t[value]
        value -= 1
        ptr, _ = _subtree(t, ptr, value, levels, li)
        ptr -= 1
        t[ptr] = (4 << 16) | (term0 >> 16)
    else:
        term0 = t[value]
        value -= 1
        term1 = t[value]
        value -= 1
        ptr -= 1
        t[ptr] = ((term1 >> 16) << 16) | (term0 >> 16)
    return ptr, li


def huffman_decoding(lengths, symbol_base=0):
    n = len(lengths)
    t = [0] * n
    counts = [0] * K_MAX_CODE_LENGTH
    codes = 0
    for i in range(n):
        ln = lengths[i]
        t[i] = ln
        if ln - 1 >= 0:
            counts[ln - 1] += 1
            codes += 1
    levels = []
    lit = codes
    for i in range(K_MAX_CODE_LENGTH):
        levels.append(lit)
        lit -= counts[i]
    base = (symbol_base << 17) + (1 << 16)
    for i in range(n):
        ln = t[i] & 0xFF
        if ln:
            levels[ln - 1] -= 1
            t[levels[ln - 1]] |= ((i << 17) + base) & M32
    if codes == 1:
        term = t[0] >> 16
        t[0] = term | (term << 16)
    elif codes > 1:
        _subtree(t, codes - 1, codes - 1, levels, 0)
    return t


def _is_valid(lengths):
    # Kraft sum must be exactly 1 (or a single code / empty)
    s = 0
    cnt = 0
    for ln in lengths:
        if ln:
            s += 1 << (K_MAX_CODE_LENGTH - ln)
            cnt += 1
    return cnt <= 1 or s == 1 << K_MAX_CODE_LENGTH


def internalize(bits, num):
    lst = list(range(K_META_CODES))
    last = 0
    out = []
    rl = 0
    while len(out) + rl < num:
        c = bits.huffman(HUFFMAN_DECODING)
        if c < 2:
            rl += rl + c + 1
        else:
            out.extend([last] * rl)
            rl = 0
            c -= 1
            lst[0] = last
            last = lst[c]
            lst[1:c + 1] = lst[0:c]
            out.append(last)
    out.extend([last] * rl)
    if len(out) > num:
        raise DeflateError("corrupt code-length table")
    return out


def inflate(data, start, out_size=None):
    """Inflate the Symbian deflate stream at data[start:]; stops at EOS or out_size."""
    bits = BitInput(data, start)
    lens = internalize(bits, K_DEFLATION_CODES)
    litlen, dist = lens[:E_LITLENS], lens[E_LITLENS:]
    if not _is_valid(litlen) or not _is_valid(dist):
        raise DeflateError("invalid huffman encoding")
    lt = huffman_decoding(litlen)
    dt = huffman_decoding(dist, K_DIST_CODE_BASE)
    out = bytearray()
    tree = lt
    ln = 0
    while out_size is None or len(out) < out_size:
        val = bits.huffman(tree) - E_LITERALS
        if val < 0:
            out.append(val & 0xFF)
            continue
        if val == E_EOS - E_LITERALS:
            break
        code = val & 0xFF
        if code >= 8:
            xtra = (code >> 2) - 1
            code -= xtra << 2
            code <<= xtra
            code |= bits.read(xtra)
        if val < K_DIST_CODE_BASE - E_LITERALS:
            ln = code + K_MIN_LENGTH
            tree = dt
            continue
        src = len(out) - (code + 1)
        if src < 0:
            raise DeflateError("distance before start")
        for _ in range(ln):
            out.append(out[src])
            src += 1
        tree = lt
    return bytes(out)

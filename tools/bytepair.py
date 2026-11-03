"""Byte-pair compression codec for E32Image (compression_type 0x102822AA).

Two directions, both RE'd from EKA2L1 @47b4ad76a:

* `decompress()` is a *faithful* port of `common::bytepair_decompress`
  (src/emu/common/src/bytepair.cpp) — the exact goto-state-machine, so it
  reproduces the emulator's decode bit-for-bit. This is our RE lens on real
  firmware: pull a compressed on-disk E32Image off the device and read its
  real code/relocations.
* `compress_page()` / `write_pages()` produce output the emulator's
  `ibytepair_stream` accepts. Byte-pair encoding is NOT canonical — Nokia's
  petran and ours will pick different substitutions — so "firmware-identical"
  is impossible and not the goal here; format-correct + round-trip-verified is.
  We emit a genuine greedy BPE (it actually shrinks) that decodes back to the
  input under both our `decompress()` and the emulator's decompressor.

Page framing (per `ibytepair_stream::read_pages`, bytepair.cpp:207):
  a *section* is [index_table][page 0][page 1]...  where index_table is
    int32 size_of_data          # sum of compressed page sizes
    int32 decompressed_size
    uint16 number_of_pages
    uint16 page_size[number_of_pages]   # compressed size of each page
  Each page decompresses to <= BYTEPAIR_PAGE_SIZE (4096) bytes.

Per-page compressed buffer (per `bytepair_decompress`, bytepair.cpp:32):
    uint8  total_pair                       # number of pair substitutions
    if total_pair:
      uint8 marker                          # escape byte (unused value)
      if total_pair < 32:  total_pair * (uint8 token, uint8 p1, uint8 p2)
      else:                32-byte bitmask of defined tokens, then their (p1,p2)
    token stream ...                        # BPE tokens, one byte each
"""

from __future__ import annotations

import struct

PAGE_SIZE = 4096  # BYTEPAIR_PAGE_SIZE (bytepair.h:45)


# ── decompressor: faithful port of common::bytepair_decompress ───────────────
def decompress(buffer: bytes, dest_size: int) -> bytes:
    """Port of bytepair.cpp:32. Returns up to dest_size decompressed bytes.

    Mirrors the C goto state-machine exactly (labels become a dispatch loop),
    so it decodes real firmware pages identically to the emulator.
    """
    data8 = 0                      # read cursor into `buffer`
    buf_end = len(buffer)
    first = list(range(256))       # lookup_table_first[b] init identity
    second = [0] * 256             # lookup_table_second
    marker = ~0 & 0xFFFFFFFF       # no byte equals this until a real marker set
    sec_stack: list[int] = []
    dest = bytearray()

    if dest_size <= 0:
        return b""

    total_pair = buffer[data8]; data8 += 1
    if total_pair:
        marker = buffer[data8]; data8 += 1
        first[marker] = (~marker) & 0xFF
        if total_pair < 32:
            pair_end = data8 + 3 * total_pair
            while data8 < pair_end:
                b = buffer[data8]; p1 = buffer[data8 + 1]; p2 = buffer[data8 + 2]
                data8 += 3
                first[b] = p1
                second[b] = p2
        else:
            mask_st = data8
            data8 += 32
            remaining = total_pair
            for b in range(0x100):
                if buffer[mask_st + (b >> 3)] & (1 << (b & 7)):
                    p1 = buffer[data8]; p2 = buffer[data8 + 1]
                    data8 += 2
                    first[b] = p1
                    second[b] = p2
                    remaining -= 1
            if remaining:
                return b""  # malformed (matches C `return 0`)

    if data8 >= buf_end:
        return b""

    b = buffer[data8]; data8 += 1
    p1 = first[b]

    # Emulate the goto structure with a small label dispatcher.
    label = "not_single" if p1 != b else "process_replace"
    while True:
        if label == "process_replace":
            if data8 >= buf_end:
                dest.append(p1)                      # done_data8
                return bytes(dest[:dest_size])
            b = buffer[data8]; data8 += 1
            dest.append(p1)
            if len(dest) >= dest_size:
                return bytes(dest[:dest_size])       # done_dest
            p1 = first[b]
            if p1 == b:
                continue                             # goto process_replace
            label = "not_single"
        if label == "not_single":
            if b == marker:
                p1 = buffer[data8]; data8 += 1       # do_marker
                label = "process_replace"
                continue
            label = "do_pair"
        if label == "do_pair":
            while True:                              # do_pair -> recurse loop
                p2 = second[b]
                b = p1
                p1 = first[b]
                sec_stack.append(p2)
                # recurse:
                while b == p1:
                    if not sec_stack:
                        break
                    b = sec_stack.pop()
                    dest.append(p1)
                    p1 = first[b]
                if b == p1 and not sec_stack:
                    label = "process_replace"
                    break
                # else b != p1 -> loop back to do_pair
            continue


# ── compressor: genuine greedy BPE, emulator-decodable ───────────────────────
def compress_page(data: bytes, *, enable_pairs: bool = True) -> bytes:
    """Compress <= PAGE_SIZE bytes into one byte-pair page buffer.

    Greedy: repeatedly substitute the most frequent adjacent pair with an
    unused byte value, until no free value remains or no pair repeats. Pair
    tokens are chosen from byte values ABSENT in the data, so an original data
    byte is never mistaken for a token and the marker escape is never needed
    (marker is set to a reserved unused value, harmless).
    """
    assert len(data) <= PAGE_SIZE, f"page too big: {len(data)}"
    tokens = list(data)
    used = set(tokens)
    free = [v for v in range(256) if v not in used]

    pairs: list[tuple[int, int, int]] = []  # (token, p1, p2)
    marker = None
    if enable_pairs and free:
        marker = free.pop(0)  # reserve one unused value as the (harmless) marker
        used.add(marker)

    # total_pair is a u8 and >=32 switches to the bulkier bitmask form, so keep
    # substitutions in the compact triple range when it still pays off.
    while free and len(pairs) < 255:
        counts: dict[tuple[int, int], int] = {}
        for i in range(len(tokens) - 1):
            pr = (tokens[i], tokens[i + 1])
            counts[pr] = counts.get(pr, 0) + 1
        if not counts:
            break
        best, cnt = max(counts.items(), key=lambda kv: kv[1])
        if cnt < 2:
            break  # a substitution only helps if the pair repeats
        tok = free.pop(0)
        pairs.append((tok, best[0], best[1]))
        used.add(tok)
        # rewrite the token stream, replacing every non-overlapping occurrence
        out: list[int] = []
        i = 0
        n = len(tokens)
        while i < n:
            if i + 1 < n and tokens[i] == best[0] and tokens[i + 1] == best[1]:
                out.append(tok); i += 2
            else:
                out.append(tokens[i]); i += 1
        tokens = out

    buf = bytearray()
    if not pairs:
        buf.append(0)  # total_pair == 0: stored page (token stream == raw)
    else:
        buf.append(len(pairs))
        buf.append(marker if marker is not None else 0)
        if len(pairs) < 32:
            for tok, p1, p2 in pairs:
                buf += bytes((tok, p1, p2))
        else:
            mask = bytearray(32)
            table = {}
            for tok, p1, p2 in pairs:
                mask[tok >> 3] |= 1 << (tok & 7)
                table[tok] = (p1, p2)
            buf += mask
            for tok in range(0x100):  # decoder walks tokens in ascending order
                if tok in table:
                    buf += bytes(table[tok])
    buf += bytes(tokens)
    return bytes(buf)


def compress_section(data: bytes, *, enable_pairs: bool = True) -> tuple[bytes, list[bytes]]:
    """Split into <=4096-byte pages and compress each. Returns (index_table, pages)."""
    pages = [compress_page(data[i:i + PAGE_SIZE], enable_pairs=enable_pairs)
             for i in range(0, len(data), PAGE_SIZE)] or [compress_page(b"")]
    size_of_data = sum(len(p) for p in pages)
    header = struct.pack("<iiH", size_of_data, len(data), len(pages))
    header += struct.pack(f"<{len(pages)}H", *(len(p) for p in pages))
    return header, pages


def write_section(data: bytes, *, enable_pairs: bool = True) -> bytes:
    """One compressed section: [index_table][page...] (what read_pages consumes)."""
    header, pages = compress_section(data, enable_pairs=enable_pairs)
    return header + b"".join(pages)


def read_section(buf: bytes, decompressed_size: int, off: int = 0) -> tuple[bytes, int]:
    """Mirror of ibytepair_stream::read_pages: decode one section starting at
    `off`. Returns (bytes, next_off). `decompressed_size` bounds the output the
    way the loader passes code_size / uncompressed_size."""
    size_of_data, decl_size, npages = struct.unpack_from("<iiH", buf, off)
    p = off + 10
    page_sizes = struct.unpack_from(f"<{npages}H", buf, p)
    p += 2 * npages
    out = bytearray()
    remaining = decompressed_size
    for ps in page_sizes:
        page = buf[p:p + ps]
        p += ps
        chunk = decompress(page, min(remaining, PAGE_SIZE))
        out += chunk
        remaining -= len(chunk)
    return bytes(out), p


if __name__ == "__main__":
    import os
    # Round-trip self-test: random + structured data, single- and multi-page.
    for size in (0, 1, 5, 100, 4096, 4097, 10000, 40000):
        for gen in (os.urandom,
                    lambda n: bytes((i * 7) & 0xFF for i in range(n)),
                    lambda n: b"ABABAB" * (n // 6 + 1),):
            data = gen(size)[:size]
            sect = write_section(data)
            back, end = read_section(sect, len(data))
            assert back == data, f"round-trip FAIL size={size} gen={gen}"
            assert end == len(sect), f"offset FAIL size={size}"
    # A structured page should actually compress.
    payload = (b"hello world " * 300)[:4096]
    comp = write_section(payload)
    print(f"self-test OK. sample: {len(payload)} raw -> {len(comp)} compressed "
          f"({100 * len(comp) // len(payload)}%)")

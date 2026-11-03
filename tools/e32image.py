# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""e32image -- expand a compressed E32Image (byte-pair or Symbian deflate) to its raw form, for the closure tool."""
import struct

COMPRESSION_BYTE_PAIR = 0x102822AA
COMPRESSION_DEFLATE = 0x101F7AFC


def decompress_e32(image: bytes) -> bytes:
    """The raw image with compression_type and the uncompressed size cleared (header offsets 28 and 124)."""
    ctype = struct.unpack_from("<I", image, 28)[0]
    if ctype == 0:
        return image
    code_offset = struct.unpack_from("<I", image, 100)[0]
    usize = struct.unpack_from("<I", image, 124)[0]
    prologue = bytearray(image[:code_offset])
    struct.pack_into("<I", prologue, 28, 0)
    struct.pack_into("<I", prologue, 124, 0)
    if ctype == COMPRESSION_DEFLATE:
        from e32deflate import inflate
        body = inflate(image, code_offset, usize)
        if len(body) != usize:
            raise ValueError(f"deflate: got {len(body)} bytes, header says {usize}")
        return bytes(prologue) + body
    if ctype != COMPRESSION_BYTE_PAIR:
        raise NotImplementedError(f"compression_type 0x{ctype:08X} unsupported")
    from bytepair import read_section
    code_size = struct.unpack_from("<I", image, 48)[0]
    code, off = read_section(image, code_size, code_offset)
    rest, _ = read_section(image, usize - code_size, off)
    return bytes(prologue) + code + rest

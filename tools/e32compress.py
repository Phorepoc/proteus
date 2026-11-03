"""e32compress -- byte-pair compress an E32Image (Proteus test images: the loader must decompress them).
    python3 e32compress.py <in> <out>
"""
import struct
import sys
import zlib
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bytepair import write_section

COMPRESSION_BYTE_PAIR = 0x102822AA
KIMAGE_CRC_INITIALISER = 0xC90FDAA2


def compress_e32(image: bytes) -> bytes:
    """Prologue (header + V-section, `code_offset` bytes) verbatim, then the code section and "the rest"
    (imports, relocations ...) as two byte-pair sections; compression type @28, uncompressed size @124, header CRC."""
    code_size = struct.unpack_from("<I", image, 48)[0]
    code_offset = struct.unpack_from("<I", image, 100)[0]
    payload = image[code_offset:]
    out = bytearray(image[:code_offset])
    out += write_section(payload[:code_size])
    out += write_section(payload[code_size:])
    struct.pack_into("<I", out, 28, COMPRESSION_BYTE_PAIR)
    struct.pack_into("<I", out, 124, len(payload))
    struct.pack_into("<I", out, 0x14, KIMAGE_CRC_INITIALISER)
    crc = zlib.crc32(bytes(out[:code_offset]), 0xFFFFFFFF) ^ 0xFFFFFFFF
    struct.pack_into("<I", out, 0x14, crc)
    return bytes(out)


if __name__ == "__main__":
    data = compress_e32(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(data)
    print(f"{sys.argv[2]}: {len(data)} bytes")

// e32decomp.h -- OURS (Proteus P2): byte-pair + Symbian-deflate E32Image decompression (pure C++). See e32decomp.cpp.
#ifndef PROTEUS_E32DECOMP_H
#define PROTEUS_E32DECOMP_H

#include <stdint.h>

namespace proteus
{
enum { KCompressBytePair = 0x102822AA, KCompressDeflate = 0x101F7AFC };
// a compressed E32Image -> the raw one (header copied, compression type + uncompressed size cleared); malloc'd (free()), NULL + aErr on a problem
uint8_t* DecompressE32(const uint8_t* aFile, uint32_t aLen, uint32_t& aOutLen, const char*& aErr);
}

#endif

// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// bench.cpp -- host benchmark of the Proteus interpreter: loads bench.elf (bench.c) like tests/isa/harness.cpp, runs it REPS times, prints MIPS.
//   bench <bench.elf> [reps]      stdout = the guest's checksum line (must be identical across interpreter changes), stderr = the speed
#include "armcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

using proteus::CArmCpu;
static bool Svc(CArmCpu& c, uint32_t imm, void*)
{
	if (imm) return false;
	if (c.R[7] == 4) { for (uint32_t i = 0; i < c.R[2]; ++i) putchar(c.Read8(c.R[1] + i)); c.R[0] = c.R[2]; return true; }
	if (c.R[7] == 1) { c.Halt(proteus::EExited, (int32_t)c.R[0]); return true; }
	return false;
}
static uint32_t U32(const std::vector<uint8_t>& f, size_t o) { uint32_t v; memcpy(&v, &f[o], 4); return v; }
static uint16_t U16(const std::vector<uint8_t>& f, size_t o) { uint16_t v; memcpy(&v, &f[o], 2); return v; }

int main(int argc, char** argv)
{
	if (argc < 2) return 2;
	const int reps = argc > 2 ? atoi(argv[2]) : 3;
	FILE* fp = fopen(argv[1], "rb"); if (!fp) { perror(argv[1]); return 2; }
	std::vector<uint8_t> f; for (int c; (c = fgetc(fp)) != EOF;) f.push_back((uint8_t)c); fclose(fp);
	const uint32_t base = 0x10000, size = 0x400000;
	uint64_t total = 0; double secs = 0;
	for (int rep = 0; rep < reps; ++rep)
	{
		std::vector<uint8_t> mem(size);
		const uint32_t phoff = U32(f, 28), entry = U32(f, 24); const uint16_t phentsize = U16(f, 42), phnum = U16(f, 44);
		for (uint16_t i = 0; i < phnum; ++i)
		{
			const size_t ph = phoff + (size_t)i * phentsize;
			if (U32(f, ph) != 1) continue;
			memcpy(&mem[U32(f, ph + 8) - base], &f[U32(f, ph + 4)], U32(f, ph + 16));
		}
		CArmCpu* cpu = new CArmCpu;
		cpu->iHost = mem.data(); cpu->iBase = base; cpu->iSize = size; cpu->R[13] = base + size - 16; cpu->iSvc = Svc; cpu->Branch(entry);
		timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
		cpu->Run(~0ull);		// no budget: the limit arithmetic must not overflow
		clock_gettime(CLOCK_MONOTONIC, &b);
		fflush(stdout);
		if (cpu->iHalt != proteus::EExited) { fprintf(stderr, "halt %d at %08x %s pc=%08x\n", (int)cpu->iHalt, cpu->iFaultAddr, cpu->iNote, cpu->iLastPc); return 1; }
		secs += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9; total += cpu->iSteps; delete cpu;
	}
	fprintf(stderr, "bench: %llu instructions, %.3f s, %.1f MIPS\n", (unsigned long long)total, secs, total / secs / 1e6);
	return 0;
}

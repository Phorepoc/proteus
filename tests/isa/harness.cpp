// harness.cpp -- runs isa.elf (the instruction differential test, isa.c) on armcpu.cpp on the host: loads the ELF's PT_LOAD segments into one guest
// region and emulates the two Linux EABI system calls it makes (write = 4, exit = 1). Its output must equal qemu-arm's (run.sh compares them).
//   harness <isa.elf>
#include "armcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using proteus::CArmCpu;

static bool Svc(CArmCpu& aCpu, uint32_t aImm, void*)
{
	if (aImm != 0) return false;
	switch (aCpu.R[7])
	{
	case 4:
		for (uint32_t i = 0; i < aCpu.R[2]; ++i) putchar(aCpu.Read8(aCpu.R[1] + i));
		aCpu.R[0] = aCpu.R[2];
		return true;
	case 1:
		aCpu.Halt(proteus::EExited, (int32_t)aCpu.R[0]);
		return true;
	}
	return false;
}

static uint32_t U32(const std::vector<uint8_t>& f, size_t o) { uint32_t v; memcpy(&v, &f[o], 4); return v; }
static uint16_t U16(const std::vector<uint8_t>& f, size_t o) { uint16_t v; memcpy(&v, &f[o], 2); return v; }

int main(int argc, char** argv)
{
	if (argc != 2) { fprintf(stderr, "usage: harness <isa.elf>\n"); return 2; }
	FILE* fp = fopen(argv[1], "rb");
	if (!fp) { perror(argv[1]); return 2; }
	std::vector<uint8_t> f;
	for (int c; (c = fgetc(fp)) != EOF;) f.push_back((uint8_t)c);
	fclose(fp);
	if (f.size() < 52 || memcmp(&f[0], "\x7f" "ELF", 4) != 0) { fprintf(stderr, "not an ELF\n"); return 2; }
	const uint32_t base = 0x10000, size = 0x100000;
	std::vector<uint8_t> mem(size);
	const uint32_t phoff = U32(f, 28), entry = U32(f, 24);
	const uint16_t phentsize = U16(f, 42), phnum = U16(f, 44);
	for (uint16_t i = 0; i < phnum; ++i)
	{
		const size_t ph = phoff + (size_t)i * phentsize;
		if (U32(f, ph) != 1) continue;			// PT_LOAD
		const uint32_t off = U32(f, ph + 4), vaddr = U32(f, ph + 8), filesz = U32(f, ph + 16), memsz = U32(f, ph + 20);
		if (vaddr < base || vaddr - base + memsz > size - 0x10000 || off + filesz > f.size()) { fprintf(stderr, "segment outside the region\n"); return 2; }
		memcpy(&mem[vaddr - base], &f[off], filesz);
	}
	CArmCpu* cpu = new CArmCpu;
	cpu->iHost = mem.data();
	cpu->iBase = base;
	cpu->iSize = size;
	cpu->R[13] = base + size - 16;
	cpu->iSvc = Svc;
	cpu->Branch(entry);
	cpu->Run(50000000);
	fflush(stdout);
	if (cpu->iHalt != proteus::EExited || cpu->iExitCode != 0)
	{
		fprintf(stderr, "halt %d code %d at %08x: %s\n", (int)cpu->iHalt, (int)cpu->iExitCode, cpu->iFaultAddr, cpu->iNote);
		return 1;
	}
	return 0;
}

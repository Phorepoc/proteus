#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# run.sh -- the Proteus instruction differential test (in the toolchain container: arm-none-eabi-gcc, g++; qemu-arm from qemu-user):
# isa.c on qemu-arm (the reference) vs the same ELF on armcpu.cpp (harness.cpp). Identical output = PASS.
#   bash tests/isa/run.sh [work dir]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../os/src/proteus"; W=${1:-$(mktemp -d)}
mkdir -p "$W"
command -v qemu-arm >/dev/null || { echo "no qemu-arm (apt-get install qemu-user)" >&2; exit 2; }
arm-none-eabi-gcc -marm -march=armv7ve -O1 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -o "$W/isa.elf" "$HERE/isa.c"
g++ -O1 -std=c++11 -I"$SRC" -o "$W/harness" "$HERE/harness.cpp" "$SRC/armcpu.cpp" "$SRC/thumb.cpp"
qemu-arm -cpu cortex-a15 "$W/isa.elf" > "$W/qemu.txt"
"$W/harness" "$W/isa.elf" > "$W/proteus.txt"
echo "$(wc -l < "$W/qemu.txt") results"
if cmp -s "$W/qemu.txt" "$W/proteus.txt"; then echo "isa: PASS"; else
  echo "isa: FAIL -- first differences (qemu < > proteus):"; diff "$W/qemu.txt" "$W/proteus.txt" | head -40; exit 1; fi
# the base classes (data processing, load/store, LDM/STM, conditions, multiplies, branches, exclusives): the fast path coverage
arm-none-eabi-gcc -marm -march=armv7ve -O1 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -Wl,-e,_start -o "$W/base.elf" "$HERE/base.c"
qemu-arm -cpu cortex-a15 "$W/base.elf" > "$W/bqemu.txt"
"$W/harness" "$W/base.elf" > "$W/bproteus.txt" || true
echo "$(wc -l < "$W/bqemu.txt") base results"
if cmp -s "$W/bqemu.txt" "$W/bproteus.txt"; then echo "base: PASS"; else
  echo "base: FAIL -- first differences (qemu < > proteus):"; diff "$W/bqemu.txt" "$W/bproteus.txt" | head -40; exit 1; fi
# Thumb-1 (the formats, entered and left by BX/BLX from ARM)
arm-none-eabi-gcc -marm -march=armv5te -O1 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -Wl,-e,_start -o "$W/thumb.elf" "$HERE/thumb.c"
qemu-arm -cpu cortex-a15 "$W/thumb.elf" > "$W/tqemu.txt"
"$W/harness" "$W/thumb.elf" > "$W/tproteus.txt" || true
echo "$(wc -l < "$W/tqemu.txt") thumb results"
if cmp -s "$W/tqemu.txt" "$W/tproteus.txt"; then echo "thumb: PASS"; else
  echo "thumb: FAIL -- first differences (qemu < > proteus):"; diff "$W/tqemu.txt" "$W/tproteus.txt" | head -40; exit 1; fi
# VFPv2 short vectors (FPSCR.LEN/STRIDE): self-checking (qemu's A15 has none, so no reference run), on armcpu.cpp only
arm-none-eabi-gcc -marm -march=armv7ve -mfpu=vfpv3 -mfloat-abi=softfp -O1 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -Wl,-e,_start -o "$W/vfpvec.elf" "$HERE/vfpvec.c"
"$W/harness" "$W/vfpvec.elf" | tee "$W/vfpvec.txt"
grep -q "vfpvec PASS" "$W/vfpvec.txt" || { echo "vfpvec: FAIL"; exit 1; }

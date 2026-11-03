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

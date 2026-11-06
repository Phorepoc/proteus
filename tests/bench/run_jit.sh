#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# run_jit.sh -- the benchmark on the interpreter and on dynarmic (P1), same guest, same checksum. Needs the host dynarmic of
# recipes/build_dynarmic.sh in $DYN (default /w/out/host) and arm-none-eabi-gcc + g++ (ghcr.io/phorepoc/base with the volume at /w).
#   bash tests/bench/run_jit.sh [work dir] [reps]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../os/src/proteus"; W=${1:-$(mktemp -d)}; REPS=${2:-3}; DYN=${DYN:-/w/out/host}
mkdir -p "$W"
arm-none-eabi-gcc -marm -march=armv7-a -O2 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -Wl,-e,main -DROUNDS=${ROUNDS:-300} -o "$W/bench.elf" "$HERE/bench.c"
g++ -O2 -std=c++20 -DPROTEUS_JIT -I"$SRC" -I"$DYN/include" -o "$W/bench_jit" "$HERE/bench.cpp" "$SRC/armcpu.cpp" "$SRC/thumb.cpp" "$SRC/armjit.cpp" \
	-L"$DYN/lib" -ldynarmic -lmcl -lfmt -lZydis
cd "$W"
echo "== interpreter"; PROTEUS_INTERP=1 ./bench_jit bench.elf "$REPS"
echo "== dynarmic"; timeout 120 ./bench_jit bench.elf "$REPS"

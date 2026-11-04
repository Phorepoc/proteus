#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# run.sh -- the interpreter host benchmark (toolchain container: arm-none-eabi-gcc + g++). bash tests/bench/run.sh [work dir] [reps]
#   env: OPT (default -O2), PROF=gprof|perf (profile build), CXXFLAGS_EXTRA
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../os/src/proteus"; W=${1:-$(mktemp -d)}; REPS=${2:-3}
mkdir -p "$W"
arm-none-eabi-gcc -marm -march=armv7-a -O2 -ffreestanding -nostdlib -static -Wl,-Ttext=0x10000 -Wl,-e,main -o "$W/bench.elf" "$HERE/bench.c"
PGF=""; [ "${PROF:-}" = gprof ] && PGF="-pg"; [ "${PROF:-}" = perf ] && PGF="-g -fno-omit-frame-pointer"
g++ ${OPT:--O2} $PGF ${CXXFLAGS_EXTRA:-} -std=c++11 -I"$SRC" -o "$W/bench" "$HERE/bench.cpp" "$SRC/armcpu.cpp" "$SRC/thumb.cpp"
cd "$W"; ./bench bench.elf "$REPS"

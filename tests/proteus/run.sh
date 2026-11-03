#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# host test of src/proteus (the P0 ARM interpreter) in the phorepoc-x86 container (g++, LP64 host: guest addresses are offsets)
cd "$(dirname "$0")/../.."
R="$(pwd -W)"
MSYS_NO_PATHCONV=1 docker.exe run --rm -e PROTEUS_TRACE -e PROTEUS_DLLDIR -e PROTEUS_REAL_EUSER -v "$R":/repo:ro phorepoc-x86 bash -c 'cd /repo && g++ -std=c++17 -O1 -Wall -Wno-unused -o /tmp/proteus_test tests/proteus/run_proteus.cpp src/proteus/armcpu.cpp src/proteus/thumb.cpp src/proteus/proteusrt.cpp src/proteus/e32decomp.cpp src/proteus/proteusexec.cpp && /tmp/proteus_test "$@"' -- "$@"

#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# host tests of os/src/proteus (the P0 ARM interpreter + executive) in the phorepoc-x86 container (g++, LP64 host: guest addresses are offsets):
#   run_proteus.cpp   -- loads a real ARM EXE and runs it (args: exe, expected log line); --thumb = the Thumb-1 decoder snippet
#   guest_servers.cpp -- self-checking test of the guest-side servers / sessions / messages (includes proteusexec.cpp itself)
#   priorities.cpp    -- self-checking test of the guest thread priorities (priority table, run order, preemption on wake, round robin)
cd "$(dirname "$0")/../.."
R="$(pwd -W)"
MSYS_NO_PATHCONV=1 docker.exe run --rm -e PROTEUS_TRACE -e PROTEUS_DLLDIR -e PROTEUS_REAL_EUSER -v "$R":/repo:ro phorepoc-x86 bash -c '
S=os/src/proteus; LIB="$S/armcpu.cpp $S/thumb.cpp $S/proteusrt.cpp $S/e32decomp.cpp $S/proteusgl.cpp $S/bridge.cpp -lpthread"
cd /repo && g++ -std=c++17 -O1 -Wall -Wno-unused -o /tmp/guest_servers tests/proteus/guest_servers.cpp $LIB && /tmp/guest_servers || exit 1
g++ -std=c++17 -O1 -Wall -Wno-unused -o /tmp/priorities tests/proteus/priorities.cpp $LIB && /tmp/priorities || exit 1
g++ -std=c++17 -O1 -Wall -Wno-unused -o /tmp/proteus_test tests/proteus/run_proteus.cpp tests/proteus/glstub.cpp $LIB $S/proteusexec.cpp && /tmp/proteus_test "$@"' -- "$@"

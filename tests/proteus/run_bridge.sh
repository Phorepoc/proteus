#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# host test of the native-library bridge (rung R1): the ARM console EXE proteusbridge.exe with bridge.cfg "native euser.dll" against a fake
# native euser (tests/proteus/bridge_test.cpp). The EXE is built by core: ./ph arm proteusbridge (emitter/proteusbridge.exe).
#   bash tests/proteus/run_bridge.sh <path of proteusbridge.exe>
set -eu
cd "$(dirname "$0")/../.."
EXE=$(cd "$(dirname "${1:?proteusbridge.exe}")" && (pwd -W 2>/dev/null || pwd))
MSYS_NO_PATHCONV=1 docker.exe run --rm -e BRTRACE -v "$(pwd -W 2>/dev/null || pwd)":/repo:ro -v "$EXE":/x:ro phorepoc-x86 bash -c '
S=os/src/proteus; cd /repo
g++ -std=c++17 -O1 -w -o /tmp/bridge_test tests/proteus/bridge_test.cpp tests/proteus/glstub.cpp $S/armcpu.cpp $S/thumb.cpp $S/proteusrt.cpp \
  $S/e32decomp.cpp $S/proteusgl.cpp $S/bridge.cpp $S/proteusexec.cpp -lpthread && /tmp/bridge_test /x/'"$(basename "$1")"

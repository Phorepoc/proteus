#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
#
# sis.sh -- the pdk's package.yml runs it after the components: proteus.sis ($PHOREPOC_BUILD/sis/proteus.sis) for a PC image
# built without this layer. It carries the handler core's loader hands an ARM EXE to (\sys\bin\cpuhandler_arm.exe, All -TCB;
# the loader finds it on any drive), the image broker (\sys\bin\proteusimg.exe, AllFiles ProtServ) and the licence texts of
# everything linked into the handler (\resource\proteus\). No ARM system DLLs: ARM apps are meant to run against the native
# libraries (docs/compat.md "Installing Proteus").
#   PROTEUS_JIT_WORK   the dynarmic work dir of prebuild.sh (default $PHOREPOC_BUILD/proteus-jit): its upstream licence files
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${PROTEUS_JIT_WORK:-$PHOREPOC_BUILD/proteus-jit}
SRC=$(ls -d "$W"/src/dynarmic-* | head -n 1)
LIC=
add() { LIC="$LIC --file $1=!:/resource/proteus/$2"; }
add "$HERE/LICENSES/Apache-2.0.txt" Apache-2.0.txt
add "$HERE/NOTICE" NOTICE.txt
add "$HERE/SOURCE-OFFER.txt" SOURCE-OFFER.txt
add "$SRC/LICENSE.txt" dynarmic-LICENSE.txt
for e in fmt mcl Zydis zycore xbyak robin-map; do     # the externals linked or compiled in (whichever the tree has)
  f=$(ls "$SRC"/externals/$e/LICENSE* "$SRC"/externals/zydis/dependencies/$e/LICENSE* 2>/dev/null | head -n 1 || true)
  [ -n "$f" ] && add "$f" "$e-LICENSE.txt"
done
python3 "$PHOREPOC_PDK/tools/make_sis.py" cpuhandler_arm proteusimg --unsigned --version "${PHOREPOC_PACKAGE_VERSION:-1.0.0}" \
  --name "Proteus" --vendor Phorepoc $LIC --out "$PHOREPOC_BUILD/sis/proteus.sis"

#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# stage_arm.sh -- copy the ARM side of the Proteus board test into the x32 build: the ARM test programs (built with
# `phe component build -arch arm proteushello proteusdll proteusdllexe proteusfs` in an ARM build) and the ARM euser +
# efsrv (that build's device images), plus the byte-pair compressed copies P2/P2b load.
#   stage_arm.sh <ARM build dir> <x32 build dir>     -> <x32 build dir>/proteus/arm/
set -eu
ARM=$1; X32=$2; HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$X32/proteus/arm"; mkdir -p "$OUT"
for f in proteushello.exe proteusdll.dll proteusdllexe.exe proteusfs.exe; do
  src=$(find "$ARM/arm/img" -maxdepth 2 -name "$f" 2>/dev/null | head -n 1)
  [ -n "$src" ] || { echo "stage_arm: no ARM $f in $ARM (build it first)" >&2; exit 1; }
  cp "$src" "$OUT/$f"
done
for f in euser.dll efsrv.dll; do cp "$ARM/arm/device/$f" "$OUT/$f"; done
python3 "$HERE/tools/e32compress.py" "$OUT/proteusdll.dll" "$OUT/proteusdll.bp.dll"
python3 "$HERE/tools/e32compress.py" "$OUT/proteusdllexe.exe" "$OUT/proteusdllexe.bp.exe"
ls -l "$OUT"

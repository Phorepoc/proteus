#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# proteus_stage.sh -- build a Proteus run disk (P3) for one app: sparkle-inst.img (a prepared C: in the protgl-pc2 volume) with the app's
# packages / EXE and its complete ARM closure in \sys\bin\arm. The closure comes from tools/proteus_closure.py with the app's APP_EXTRAS
# (run-time loads no import names) and --cdl --strict: an unresolved DLL fails the staging instead of failing the app at run time.
# Nothing is staged by hand. Docker volumes only (no Windows bind mounts):
#   protgl-pc2 (PC build, holds the base disk)  protgl-in (inputs)  protgl-arm (ARM build: arm/img)  protgl-src (this repo at /w/proteus)
#   proteus_stage.sh NAME APP sis DIR        DIR in protgl-in holds *.sis and sisapp.txt; the closure root is the EXE sisapp.txt names, extracted from the packages
#   proteus_stage.sh NAME APP exe DIR/FILE   a loose EXE in protgl-in
#   proteus_stage.sh NAME APP installed DIR  the base disk already holds the installed app (sparkle-inst.img: Qt + Sparkle): no SIS goes on C:, DIR only names the EXE and gives the closure root
#   EXE=/in/path/app.exe  closure root for the sis mode          OUT=<dir>  where compat-NAME.img lands (default: .)
#   BASE=lg-base.img      the blank run disk in protgl-pc2 (default sparkle-inst.img, 256 MB with the Qt stack; a big package needs the 1 GB lg-base.img)
#   DATA=calc-data        a tree in protgl-in copied onto C: (registration + resources of a ROM app: the Calculator)
set -eu
export MSYS_NO_PATHCONV=1
NAME=$1; APP=$2; MODE=$3; ARG=$4
OUT=$(cd "${OUT:-.}" && pwd -W 2>/dev/null || pwd)
C=protreg2-stage-$NAME
docker rm -f "$C" >/dev/null 2>&1 || true
docker create --name "$C" -e MODE="$MODE" -e ARG="$ARG" -e APP="$APP" -e NAME="$NAME" -e EXE="${EXE:-}" -e BASE="${BASE:-}" -e DATA="${DATA:-}" \
  -v protgl-pc2:/b -v protgl-in:/in -v protgl-src:/w -v protgl-arm:/armb ghcr.io/phorepoc/base:latest bash -c '
set -e; export MTOOLS_SKIP_CHECK=1; P=1048576; D=/b/compat-$NAME.img; cp /b/${BASE:-sparkle-inst.img} $D; I=$D@@$P
mmd -i $I ::sis ::sys ::sys/bin ::sys/bin/arm 2>/dev/null || true
for f in $(mdir -b -i $I ::sis | grep -i "\.sis$"); do mdel -i $I "$f"; done
rm -rf /tmp/cl
if [ "$MODE" = sis ] || [ "$MODE" = installed ]; then
  if [ "$MODE" = sis ]; then for f in /in/$ARG/*.sis; do mcopy -o -i $I $f ::sis/; done; fi
  mcopy -o -i $I /in/$ARG/sisapp.txt ::sisapp.txt
  mkdir -p /tmp/x; for f in /in/$ARG/*.sis; do python3 /w/proteus/tools/sis_extract.py $f /tmp/x >/dev/null; done
  APPX=$(head -n1 /in/$ARG/sisapp.txt | sed "s/[[:space:]]*$//")
  ROOT=${EXE:-$(find /tmp/x -iname "$APPX" | head -1)}; test -n "$ROOT" || { echo "no $APPX in the packages"; exit 1; }
else
  mcopy -o -i $I /in/$ARG ::sys/bin/$(basename $ARG)
  printf "%s" "$(basename $ARG)" > /tmp/sa; mcopy -o -i $I /tmp/sa ::sisapp.txt
  ROOT=/in/$ARG
fi
if [ -n "$DATA" ]; then for d in $(cd /in/$DATA && find . -type d | sed "s|^./||" | grep -v "^.$"); do mmd -i $I ::$d 2>/dev/null || true; done; mcopy -s -o -i $I /in/$DATA/* ::; fi
python3 /w/proteus/tools/proteus_closure.py "$ROOT" --app $APP --dirs /armb/arm/img --cdl --strict --quiet --out /tmp/cl
mcopy -o -i $I /tmp/cl/* ::sys/bin/arm/        # -o: a stale file of an older staging must not win
echo STAGED $(ls /tmp/cl | wc -l) files' >/dev/null
docker start -a "$C"
docker cp "$C:/b/compat-$NAME.img" "$OUT/compat-$NAME.img"
docker rm "$C" >/dev/null

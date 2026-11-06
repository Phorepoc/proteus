#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# build_dynarmic.sh -- dynarmic (0BSD), the ARM-to-x86-64 translator Proteus P1 runs guest code with, as a static library.
#   ARCH=host  the build machine (LP64): the host tests and the benchmark (tests/bench)
#   ARCH=x32   the Phorepoc PC image (-mx32 on cxxrt's libstdc++), linked into proteus.exe
# Sources are downloaded once and checked against dynarmic.lock (the archived upstream's last commit) -- no copy lives here.
# Run in a container with cmake (ghcr.io/phorepoc/web), the volume holding the work dir at /w:
#   docker run --rm -v proteus-jit:/w -v <proteus>:/proteus:ro ghcr.io/phorepoc/web bash /proteus/recipes/build_dynarmic.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-host}
W=${W:-/w}
. "$HERE/../dynarmic.lock"
mkdir -p "$W/dl"
fetch() {	# url file sha256
	[ -f "$W/dl/$2" ] || curl -sSLfo "$W/dl/$2.part" "$1" && { [ -f "$W/dl/$2" ] || mv "$W/dl/$2.part" "$W/dl/$2"; }
	echo "$3  $W/dl/$2" | sha256sum -c --quiet
}
fetch "https://codeload.github.com/lioncash/dynarmic/tar.gz/$DYNARMIC_COMMIT" "dynarmic-$DYNARMIC_COMMIT.tar.gz" "$DYNARMIC_SHA256"
fetch "$BOOST_URL" "$(basename "$BOOST_URL")" "$BOOST_SHA256"

SRC="$W/src/dynarmic-$DYNARMIC_COMMIT"
if [ ! -d "$SRC" ]; then
	mkdir -p "$W/src"; tar -xzf "$W/dl/dynarmic-$DYNARMIC_COMMIT.tar.gz" -C "$W/src"
	for p in "$HERE"/../patches/dynarmic/*.patch; do [ -f "$p" ] && patch -d "$SRC" -p1 < "$p"; done
fi
BOOST="$W/src/boost"
if [ ! -d "$BOOST/boost" ]; then
	mkdir -p "$BOOST"; tar -xzf "$W/dl/$(basename "$BOOST_URL")" -C "$BOOST" --strip-components=1 --wildcards '*/boost/*'
fi

OUT="$W/out/$ARCH"; B="$W/build/$ARCH"; mkdir -p "$OUT" "$B"
case "$ARCH" in
	host) TC=() ;;
	*) echo "build_dynarmic: ARCH=$ARCH not yet" >&2; exit 2 ;;
esac
cmake -S "$SRC" -B "$B" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
	-DDYNARMIC_TESTS=${DYNARMIC_TESTS:-OFF} -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
	-DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_ENABLE_CPU_FEATURE_DETECTION=ON \
	-DBoost_INCLUDE_DIR="$BOOST" -DCMAKE_INSTALL_PREFIX="$OUT" > "$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
cmake --build "$B" -j"${JOBS:-6}" > "$B/build.log" 2>&1 || { grep -m15 -E "error|Error" "$B/build.log"; tail -5 "$B/build.log"; exit 1; }
cmake --install "$B" > /dev/null
echo "build_dynarmic: $ARCH -> $OUT"; ls "$OUT/lib"

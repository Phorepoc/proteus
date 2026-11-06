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
	x32)
		# the pdk's pure-Open-C compiler wrappers over cxxrt's libstdc++ (GCC 13, C++20), as webkit-symbian's JSC build:
		# PHOREPOC_PDK / PHOREPOC_CORE / PHOREPOC_BUILD as for every pdk tool, cxxrt built for x32 in $PHOREPOC_BUILD
		CX=${CX:-$PHOREPOC_BUILD/cxxrt-work/lsc-x32}
		GCC_SRC=$(ls -d "$CX"/gcc-* | head -1)
		PP="$W/pp-x32"
		ARCH=x32 W="$PP" bash "$PHOREPOC_PDK/tools/toolchain/mk_pure_wrappers.sh" > /dev/null
		CXXINC="-I$CX/build/include/x86_64-unknown-elf -I$CX/build/include -I$GCC_SRC/libstdc++-v3/libsupc++"
		cat > "$B/x32.cmake" <<-EOF
		set(CMAKE_SYSTEM_NAME Linux)
		set(CMAKE_SYSTEM_PROCESSOR x86_64)
		set(CMAKE_C_COMPILER $PP/bin/pp-gcc)
		set(CMAKE_CXX_COMPILER $PP/bin/pp-g++)
		set(CMAKE_ASM_COMPILER $PP/bin/pp-gcc)
		set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
		set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE BOOL "")
		set(CMAKE_USE_PTHREADS_INIT 1 CACHE BOOL "")
		set(CMAKE_THREAD_LIBS_INIT "" CACHE STRING "")
		set(CMAKE_C_FLAGS_INIT "-U__linux__ -U__linux -U__gnu_linux__ -Ulinux -DPHOREPOC=1")
		set(CMAKE_CXX_FLAGS_INIT "-U__linux__ -U__linux -U__gnu_linux__ -Ulinux -DPHOREPOC=1 $CXXINC -fexceptions -frtti -include stdint.h -DDYNARMIC_EXTERNAL_CODE_MEMORY=1 -DBOOST_PLATFORM_CONFIG=\\\\\\"boost/config/platform/linux.hpp\\\\\\"")
		# (stdint.h first: Open C wchar.h defines WCHAR_MIN as a cast, which Boost tests in #if; Boost: __SYMBIAN32__ selects its S60 3rd-edition config, which rejects us; the POSIX one fits Open C)
		EOF
		TC=(-DCMAKE_TOOLCHAIN_FILE="$B/x32.cmake" -DDYNARMIC_GENERIC_EXCEPTION_HANDLER=ON)	# no ucontext: no fastmem (patch 0003)
		;;
	*) echo "build_dynarmic: ARCH=$ARCH: host or x32" >&2; exit 2 ;;
esac
cmake -S "$SRC" -B "$B" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
	-DDYNARMIC_TESTS=${DYNARMIC_TESTS:-OFF} -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
	-DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_ENABLE_CPU_FEATURE_DETECTION=ON \
	-DBoost_INCLUDE_DIR="$BOOST" -DCMAKE_INSTALL_PREFIX="$OUT" > "$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
cmake --build "$B" -j"${JOBS:-6}" > "$B/build.log" 2>&1 || { grep -m15 -E "error|Error" "$B/build.log"; tail -5 "$B/build.log"; exit 1; }
cmake --install "$B" > /dev/null
if [ "$ARCH" = x32 ]; then
	# Proteus's side of the translator (armjit.cpp: C++20 over dynarmic's headers) as an archive, then everything where the registry's
	# x32_archives look (registry/components_proteus.py): $PHOREPOC_BUILD/thirdparty/x86/dynarmic/
	PSRC="$HERE/../os/src/proteus"
	"$PP/bin/pp-g++" -std=c++20 -O2 $CXXINC -include stdint.h -fexceptions -frtti -U__linux__ -DPROTEUS_JIT -I"$PSRC" -I"$OUT/include" \
		-c "$PSRC/armjit.cpp" -o "$B/armjit.o"
	ar rcs "$OUT/lib/libproteusjit.a" "$B/armjit.o"
	TPD="$PHOREPOC_BUILD/thirdparty/x86/dynarmic"; mkdir -p "$TPD"
	cp "$OUT"/lib/lib{proteusjit,dynarmic,mcl,fmt,Zydis}.a "$TPD/"
fi
echo "build_dynarmic: $ARCH -> $OUT"; ls "$OUT/lib"

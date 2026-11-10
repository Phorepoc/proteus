#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
#
# prebuild.sh -- the pdk's package.yml runs it in the toolchain container before the components: cxxrt's x32 archives, then
# dynarmic + the JIT glue for x32 (build_dynarmic.sh), which cpuhandler_arm links ($PHOREPOC_BUILD/thirdparty/x86/dynarmic/).
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
[ "${PHOREPOC_ARCH:-x32}" = x32 ] || { echo "proteus: the handler is an x32 program (PHOREPOC_ARCH=$PHOREPOC_ARCH)"; exit 1; }
IFS=: read -r -a repos <<< "${PHOREPOC_ADAPTATIONS:-}"
IFS=: read -r -a names <<< "${PHOREPOC_ADAPTATION_NAMES:-}"
cxxrt=
for i in "${!repos[@]}"; do [ "${names[i]:-}" = cxxrt ] && cxxrt=${repos[i]}; done
[ -n "$cxxrt" ] || { echo "proteus needs cxxrt in PHOREPOC_ADAPTATIONS"; exit 1; }
bash "$cxxrt/build.sh"
ARCH=x32 W=${PROTEUS_JIT_WORK:-$PHOREPOC_BUILD/proteus-jit} exec bash "$HERE/recipes/build_dynarmic.sh"

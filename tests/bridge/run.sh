#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# Rung R0 checks, in the clang container (tools/bridge/Dockerfile.tools):  bash tests/bridge/run.sh <core checkout>
#  1. the generated tables compile and are complete  2. layoutcheck: ARM vs x32 layouts  3. the layout static_asserts hold
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=${1:?core checkout}
cd "$HERE/../.."
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD":/proteus proteus-bridge-tools sh -c \
  'clang++ -std=c++11 -O0 -Wall -o /tmp/t /proteus/tests/bridge/bridge_inc_test.cpp && /tmp/t && echo "bridge tables: PASS"'
bash tools/bridge/run.sh "$CORE" layoutcheck.py
bash tools/bridge/run.sh "$CORE" ../../tests/bridge/layout_asserts.py

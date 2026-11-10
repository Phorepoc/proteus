#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# Run a tools/bridge python tool in the clang container.  usage: tools/bridge/run.sh <core checkout> <script.py> [args...]
# Image: docker build -t proteus-bridge-tools -f tools/bridge/Dockerfile.tools tools/bridge   (debian:trixie + clang + python3)
set -euo pipefail
CORE="$1"; shift
HERE="$(cd "$(dirname "$0")/../.." && (pwd -W 2>/dev/null || pwd))"
CORE="$(cd "$CORE" && (pwd -W 2>/dev/null || pwd))"
SCRIPT="$1"; shift
MSYS_NO_PATHCONV=1 docker run --rm -v "$CORE":/repo:ro -v "$HERE":/proteus ${BRIDGE_EXTRA_MOUNT:-} proteus-bridge-tools \
    python3 -B "/proteus/tools/bridge/$SCRIPT" --repo /repo "$@"

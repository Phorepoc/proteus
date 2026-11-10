#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""Compile the generated static_asserts (bridge_<dll>.inc, BRIDGE_LAYOUT_ASSERTS) against the 2010 headers on the x32
target: the native layout of every class with exported members equals the ARM one an app compiled in.
In the clang container:  tools/bridge/run.sh <core checkout> ../../tests/bridge/layout_asserts.py"""
import argparse
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "bridge"))
import symabi_lib as L  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--repo", required=True)
a = ap.parse_args()
GEN = os.path.join(HERE, "..", "..", "os", "src", "proteus", "gen")
bad = 0
for dll in ("euser", "efsrv"):
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, f"asserts_{dll}.cpp")
        with open(p, "w") as f:
            f.write(f'#define BRIDGE_LAYOUT_ASSERTS 1\n#include "{os.path.join(HERE, "..", "..", "tools", "bridge", "umbrella_" + dll + ".cpp")}"\n')
            f.write('#include <stdint.h>\nstruct CArmCpu; struct TBridgeExport;\n')
            f.write(f'#include "{os.path.join(GEN, "bridge_" + dll + ".inc")}"\n')
        try:
            # the table part needs TBridgeExport: compile only the assert block
            src = open(os.path.join(GEN, f"bridge_{dll}.inc")).read()
            src = src[src.index("#ifdef BRIDGE_LAYOUT_ASSERTS"):]
            with open(p, "w") as f:
                f.write(f'#include "{os.path.join(HERE, "..", "..", "tools", "bridge", "umbrella_" + dll + ".cpp")}"\n#define BRIDGE_LAYOUT_ASSERTS 1\n{src}\n')
            L.clang(a.repo, "x32", dll, [], src=p)
            print(f"{dll}: layout static_asserts hold on x32 ({src.count('BrLayout_')})")
        except SystemExit as e:
            print(f"{dll}: FAIL {e}")
            bad = 1
sys.exit(bad)

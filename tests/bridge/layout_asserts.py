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
# "non-trivial for calls" (abi/<dll>.json `ntcall`, the hidden-pointer return) must be the x32 compiler's decision too: clang's
# canPassInRegisters on the x32 target for every class the dump has. Pinned: the TBuf<N> bases are trivial (the x32 GCC euser/efsrv
# return TBuf<2> = TDriveUnit::Name in rax:rdx), the polymorphic CBase is not
import json  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    for dll in ("euser", "efsrv"):
        x32 = L.Model().build(L.load_ast(a.repo, "x32", dll, tmp)).records
        cls = json.load(open(os.path.join(HERE, "..", "..", "tools", "bridge", "abi", dll + ".json")))["classes"]
        diff = [c for c, r in cls.items() if c in x32 and bool(r.get("ntcall")) != bool(x32[c].get("ntcall"))]
        pins = {"euser": {"TBufBase16": False, "TBufBase8": False, "TDes16": False, "TPtr8": False, "CBase": True},
                "efsrv": {"TDriveUnit": False}}[dll]
        wrong = [c for c, v in pins.items() if bool(cls.get(c, x32.get(c, {})).get("ntcall")) != v]
        print(f"{dll}: ntcall ARM dump vs x32: {len(diff)} differences {diff[:8]}, pins {'PASS' if not wrong else 'FAIL ' + str(wrong)}")
        bad |= bool(diff or wrong)
sys.exit(bad)

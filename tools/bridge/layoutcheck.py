#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""layoutcheck -- dump every class with exported members twice (ARM EABI: armv5te, aapcs, 4-byte enums; and x32) and diff
sizeof, alignof, base offsets, field offsets and vtable slot order. An ARM app was compiled against the first, the
native library against the second: any difference is a bug the bridge cannot paper over. Exit status 1 on a difference.

    python3 tools/bridge/layoutcheck.py --repo <core checkout> [--dll euser --dll efsrv] [--json out.json]
    (in the clang container: tools/bridge/run.sh <core> layoutcheck.py)
Classes come from tools/bridge/abi/<dll>.json (every class an export belongs to, plus the by-value structs).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symabi_lib as L  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def vtables(repo: str, target: str, dll: str, tmp: str, want: list[str]) -> dict[str, list[str]]:
    model = L.Model().build(L.load_ast(repo, target, dll, tmp))
    memo: dict = {}
    return {q: L.vtable_slots(model, q, memo) for q in want if q in model.records}


def diff(arm: dict, x32: dict, vt_arm: dict, vt_x32: dict, classes: list[str]) -> list[str]:
    out = []
    for c in classes:
        a, x = arm.get(c), x32.get(c)
        if a is None or x is None:
            if (a is None) != (x is None):
                out.append(f"{c}: laid out on one target only")
            continue
        for k in ("size", "align", "nvsize", "vptr"):
            if a.get(k) != x.get(k):
                out.append(f"{c}: {k} arm={a.get(k)} x32={x.get(k)}")
        ba, bx = [(b["name"], b["off"]) for b in a["bases"]], [(b["name"], b["off"]) for b in x["bases"]]
        if ba != bx:
            out.append(f"{c}: bases arm={ba} x32={bx}")
        fa, fx = {(f["name"]): f["off"] for f in a["fields"]}, {(f["name"]): f["off"] for f in x["fields"]}
        for n in sorted(set(fa) | set(fx)):
            if fa.get(n) != fx.get(n):
                out.append(f"{c}.{n}: offset arm={fa.get(n)} x32={fx.get(n)}")
        if vt_arm.get(c) != vt_x32.get(c):
            out.append(f"{c}: vtable arm={vt_arm.get(c)} x32={vt_x32.get(c)}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--dll", action="append")
    ap.add_argument("--json")
    a = ap.parse_args()
    total, diffs, res = 0, [], {}
    for dll in a.dll or ["euser", "efsrv"]:
        abi = json.load(open(os.path.join(HERE, "abi", dll + ".json")))
        classes = [c for c, r in abi["classes"].items() if "<" not in c and r.get("declared", True)]
        with tempfile.TemporaryDirectory() as tmp:
            arm = L.layouts(a.repo, "arm", dll, classes, tmp)
            x32 = L.layouts(a.repo, "x32", dll, classes, tmp)
            va, vx = vtables(a.repo, "arm", dll, tmp, classes), vtables(a.repo, "x32", dll, tmp, classes)
        every = sorted(set(arm) | set(x32))
        d = diff(arm, x32, va, vx, every)
        # each class with exported members, laid out on both targets
        have = [c for c in classes if c in arm and c in x32]
        print(f"{dll}: {len(classes)} classes with exported members ({len(have)} laid out on both), "
              f"{len(every)} records compared incl. bases and members, {len(d)} differences")
        for ln in d:
            print("  DIFF", ln)
        total += len(every)
        diffs += [f"{dll}: {x}" for x in d]
        res[dll] = {"classes": len(classes), "laid_out_both": len(have), "records": len(every), "diffs": d}
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()

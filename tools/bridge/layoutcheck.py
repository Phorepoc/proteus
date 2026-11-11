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
import re
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


# the C libraries (Open C, round 16) have no classes: what an ARM caller and the native callee must agree on are the types their
# exports name (through pointers): every record and typedef spelled in a parameter or result, function-pointer types aside
C_DLLS = ("libc", "libpthread")
_SCALAR = re.compile(r"^(unsigned |signed )?(int|char|short|long|long long|void|double|float|long double|bool|_Bool|__e32_va_list)$")


def c_types(abi: dict) -> list[str]:
    out = set()
    for e in abi["exports"]:
        for p in (e.get("params") or []) + [e.get("ret") or {}]:
            t = re.sub(r"\b(const|volatile|restrict|__restrict)\b", "", p.get("t", ""))
            t = t.replace("*", "").replace("&", "").strip()
            t = re.sub(r"\s+", " ", t)
            if not t or "(" in t or "__attribute__" in t or _SCALAR.match(t):
                continue
            out.add(t)
    return sorted(out)


def complete_types(repo: str, dll: str, types: list[str]) -> list[str]:
    """drop the opaque ones (DIR, WDIR, iconv_t's target ...): the guest only passes their pointers around"""
    with tempfile.TemporaryDirectory() as tmp:
        for _ in range(4):
            err = L.clang(repo, "arm", dll, ["-ferror-limit=0"], src=L.force_tu(types, dll, tmp), allow_fail=True)
            bad = {int(i) for i in re.findall(r"forced_(\d+) = sizeof", err)}
            if not bad:
                break
            types = [t for i, t in enumerate(types) if i not in bad]
    return types


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--dll", action="append")
    ap.add_argument("--json")
    a = ap.parse_args()
    total, diffs, res = 0, [], {}
    for dll in a.dll or ["euser", "efsrv", "libc", "libpthread"]:
        abi = json.load(open(os.path.join(HERE, "abi", dll + ".json")))
        classes = [c for c, r in abi["classes"].items() if "<" not in c and r.get("declared", True)]
        sizes = []
        if dll in C_DLLS:
            classes = complete_types(a.repo, dll, c_types(abi))
        with tempfile.TemporaryDirectory() as tmp:
            arm = L.layouts(a.repo, "arm", dll, classes, tmp)
            x32 = L.layouts(a.repo, "x32", dll, classes, tmp)
            va, vx = vtables(a.repo, "arm", dll, tmp, classes), vtables(a.repo, "x32", dll, tmp, classes)
            if dll in C_DLLS:	# sizeof/alignof of every named type (a typedef of a scalar too: off_t, fpos_t, pthread_t ...)
                pa, px = L.probe_sizes(a.repo, "arm", dll, classes, tmp), L.probe_sizes(a.repo, "x32", dll, classes, tmp)
                sizes = [f"{t}: sizeof/alignof arm={pa.get(t)} x32={px.get(t)}" for t in classes if pa.get(t) != px.get(t)]
        every = sorted(set(arm) | set(x32))
        d = sizes + diff(arm, x32, va, vx, every)
        # each class with exported members, laid out on both targets
        have = [c for c in classes if c in arm and c in x32]
        print(f"{dll}: {len(classes)} {'named types' if dll in C_DLLS else 'classes with exported members'} ({len(have)} laid out on both), "
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

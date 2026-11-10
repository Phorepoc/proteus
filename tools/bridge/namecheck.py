#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""namecheck -- the bridge binds an ARM ordinal to the native export by NAME, never by native ordinal. Report the EABI .def
names (not ABSENT) that our native x32 euser/efsrv do not export. The native spelling differs from the EABI one in one
place: va_list (ARM `St9__va_list`, x32 `P13__va_list_tag`), compared after that rewrite. Exit 1 if any name is missing.

    python3 tools/bridge/namecheck.py --repo <core checkout> [--native <dir with euser-x32/euser_exports.json ...>]
The native lists are core's build output (.phorepoc-x86-build/<dll>-x32/<dll>_exports.json; gitignored there)."""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symabi_lib as L  # noqa: E402


def x86_name(n: str) -> str:
    return n.replace("St9__va_list", "P13__va_list_tag")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--native")
    ap.add_argument("--json")
    a = ap.parse_args()
    nat_dir = a.native or os.path.join(a.repo, ".phorepoc-x86-build")
    bad, res = 0, {}
    for dll in ("euser", "efsrv"):
        nat = json.load(open(os.path.join(nat_dir, f"{dll}-x32", f"{dll}_exports.json")))["ordinals"]
        native = {v[0] for v in nat.values() if v[0]}
        arm = [(o, n, f) for o, n, f, _ in L.read_def(os.path.join(a.repo, L.DEFS[dll]))]
        live = [(o, n) for o, n, f in arm if "ABSENT" not in f]
        miss = [(o, n) for o, n in live if x86_name(n) not in native]
        extra = sorted(native - {x86_name(n) for o, n, f in arm})
        print(f"{dll}: {len(live)} ARM names (+{len(arm) - len(live)} ABSENT), {len(native)} native names; "
              f"on ARM but not native: {len(miss)}; native only: {len(extra)}")
        for o, n in miss:
            print(f"  MISSING ordinal {o}: {n}")
        res[dll] = {"arm": len(live), "native": len(native), "missing": [n for o, n in miss], "native_only": extra}
        bad += len(miss)
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

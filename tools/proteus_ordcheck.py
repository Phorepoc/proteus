# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""proteus_ordcheck -- Proteus P2: does our 2010-source ARM DLL set serve the ordinals a real (Belle) ARM EXE imports?

For every DLL the EXE imports: the ordinals it uses, the export count of the Belle DLL (from a firmware Z: dump, if given) and of OUR ARM
build (emitter/<name>, euser/efsrv -> the *_dev device builds), and per used ordinal: MISSING (beyond our export table) or STUB (our table
entry is the build's unimplemented-export stub, emitter/<dll>.meta "stub=0x..."). A used ordinal that exists in both but whose MEANING
changed between 2010 and Belle cannot be seen here (no Belle .def); count differences flag the DLLs where that is possible.

    py tools/proteus_ordcheck.py <arm exe> [--belle <dir with the Belle sys/bin files>] [--ours emitter]
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "emitter"))
from e32image import decompress_e32  # noqa: E402

ALIAS = {"euser.dll": "euser_dev.dll", "efsrv.dll": "efsrv_dev.dll"}


def load(path):
    with open(path, "rb") as f:
        return decompress_e32(f.read())


def exports(img):
    n, off = struct.unpack_from("<I", img, 92)[0], struct.unpack_from("<I", img, 88)[0]
    return list(struct.unpack_from(f"<{n}I", img, off)) if n and off else []


def imports(img):
    off = struct.unpack_from("<I", img, 108)[0]
    refs = struct.unpack_from("<I", img, 84)[0]
    code_off = struct.unpack_from("<I", img, 100)[0]
    out, p = [], off + 4
    for _ in range(refs):
        name_off, cnt = struct.unpack_from("<II", img, p)
        p += 8
        name = img[off + name_off:img.index(b"\0", off + name_off)].decode()
        ords = set()
        for _ in range(cnt):
            w = struct.unpack_from("<I", img, code_off + struct.unpack_from("<I", img, p)[0])[0]
            ords.add(w & 0xFFFF)
            p += 4
        out.append((name, sorted(ords)))
    return out


def base(name):
    return re.sub(r"[{\[].*?[}\]]", "", name).lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--belle")
    ap.add_argument("--ours", default=os.path.join(os.path.dirname(__file__), "..", "emitter"))
    a = ap.parse_args()
    bad = 0
    for name, ords in imports(load(a.exe)):
        b = base(name)
        ours_p = os.path.join(a.ours, ALIAS.get(b, b))
        belle_n = "-"
        if a.belle and os.path.exists(os.path.join(a.belle, b)):
            belle_n = len(exports(load(os.path.join(a.belle, b))))
        if not os.path.exists(ours_p):
            print(f"{b:28} uses {len(ords):4}  belle {belle_n!s:>5}  ours  ABSENT")
            bad += 1
            continue
        ex = exports(load(ours_p))
        stub = None
        meta = ours_p + ".meta"
        if os.path.exists(meta):
            m = re.search(r"stub=0x([0-9a-fA-F]+)", open(meta).read())
            stub = int(m.group(1), 16) if m else None
        code_base = struct.unpack_from("<I", load(ours_p), 76)[0]
        missing = [o for o in ords if o > len(ex)]
        stubs = [o for o in ords if o <= len(ex) and stub is not None and ex[o - 1] - code_base == stub]
        flag = "" if belle_n in ("-", len(ex)) else "  COUNT DIFFERS"
        print(f"{b:28} uses {len(ords):4}  belle {belle_n!s:>5}  ours {len(ex):5}  missing {missing or '-'}  stub {stubs or '-'}{flag}")
        bad += bool(missing or stubs)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())

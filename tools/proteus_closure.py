# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""proteus_closure -- Proteus: collect an ARM app's DLL closure from our ARM builds and report its Thumb content.

Walks the imports of <exe> transitively, resolving each DLL in --dirs (default: the EXE's own directory, then emitter/; euser/efsrv/hal map
to the *_dev device builds, the ones the real-euser path runs), and optionally copies everything into --out (for
`DISKFILES=<out>=proteus ./ph boot`, then `proteus.exe -r c:\\proteus\\<exe>`; P2e in proteus.exe runs c:\\proteus\\about.exe).

Thumb report per module (Proteus' interpreter is ARM-only): exports whose address has bit 0 set (Thumb entry points) and a
heuristic count of Thumb BL/BLX halfword pairs vs ARM condition-0xE words (see thumb_stats). Unresolved DLLs are listed (they stay HLE traps).

    py tools/proteus_closure.py <arm exe> [--dirs d1,d2] [--out dir] [--quiet] [--extra a.dll,b.dll] [--cdl]
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e32image import decompress_e32  # noqa: E402

ALIAS = {"euser.dll": "euser_dev.dll", "efsrv.dll": "efsrv_dev.dll", "hal.dll": "hal_dev.dll"}
REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))


def load(path):
    with open(path, "rb") as f:
        return decompress_e32(f.read())


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def import_names(img):
    off, refs = u32(img, 108), u32(img, 84)
    out, p = [], off + 4
    for _ in range(refs):
        name_off, cnt = struct.unpack_from("<II", img, p)
        p += 8 + 4 * cnt
        out.append(img[off + name_off:img.index(b"\0", off + name_off)].decode())
    return out


def base(name):
    return re.sub(r"[{\[].*?[}\]]", "", name).lower()


def thumb_stats(img):
    """(Thumb exports, exports, Thumb BL/BLX pairs, ARM 'always' words) over the text section. A Thumb BL/BLX is the halfword pair
    F000-F7FF + E800-FFFF; ARM code is dominated by condition 0xE words. Heuristic (literal pools count too), but the two numbers
    separate ARM modules (pairs ~0) from Thumb ones (pairs >> 0) clearly."""
    n, eo = u32(img, 92), u32(img, 88)
    ex = struct.unpack_from(f"<{n}I", img, eo) if n and eo and eo + 4 * n <= len(img) else ()
    co, cs, ts = u32(img, 100), u32(img, 48), u32(img, 96) or u32(img, 48)
    end = co + min(ts, cs)
    pairs = arm = 0
    for o in range(co, end - 3, 2):
        h0, h1 = struct.unpack_from("<HH", img, o)
        if 0xF000 <= h0 <= 0xF7FF and h1 >= 0xE800:
            pairs += 1
    for o in range(co, end - 3, 4):
        if img[o + 3] >> 4 == 0xE:
            arm += 1
    return sum(1 for e in ex if e & 1), len(ex), pairs, arm


# Run-time loads (RLibrary::Load / ECOM / CDL) that no import names, per app. Found by the run-disk regressions: each missing one cost a
# silent failure (`RLibrary::Load f32agent.dll -> -1` = no music; no ICL plug-ins = no images; a stale stub of iaupdateapi/drtrvct2_2
# = `proteus wait 1`, no window group). The GL DLLs are not listed on purpose: they run natively (NATIVE_GL).
APP_EXTRAS = {
    # scdv: the ARM screen driver the window server loads (the OMAP draw devices; the generic one refuses the rotation). The Avkon UI's
    # own run-time loads (every Avkon app: akninit at CEikonEnv construction, the CDL/FEP plug-ins): before 2026-11-10 a run disk often
    # had them from an earlier staging; a plain disk does not (PyS60: `RLibrary::Load Z:\sysinkninit.dll -> -1`, User::Exit(-1))
    "common": ["scdv.dll", "akninit.dll", "2001e2b1.dll", "eikcdlg.dll", "avkonfep.dll", "aknfepuiavkonplugin.dll"],
    "sparkle": ["f32agent.dll", "f32agentui.dll"],
    "letsgolf2": ["akninit.dll", "2001e2b1.dll", "eikcdlg.dll", "bitmaptransformsrefplugin.dll", "avkonfep.dll", "aknfepuiavkonplugin.dll",
                  "bmpcodec.dll", "gifcodec.dll", "icocodec.dll", "jpegcodec.dll", "mbmcodec.dll", "pngcodec.dll", "wbmpcodec.dll",
                  "imageconversionresolver.dll", "f32agent.dll", "f32agentui.dll"],
    "pys60": ["iaupdateapi.dll", "drtrvct2_2.dll"],
    "calculator": [],
}
# euser/efsrv/hal come with the ROM (proteus/arm: the device builds P2c/P2d run), never from the run disk
ROM_ARM = {"euser.dll", "efsrv.dll", "hal.dll"}
NATIVE_GL = {"libegl.dll", "libglesv2.dll", "libglesv1_cm.dll", "libgles_cm.dll"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--dirs")
    ap.add_argument("--out")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--strict", action="store_true", help="exit 2 when a DLL stays unresolved (a stale or incomplete ARM build: the app then fails at run time, e.g. drtrvct2_2 ordinal 28 not emulated)")
    ap.add_argument("--extra", help="comma-separated DLLs the app loads at run time (RLibrary::Load: ECOM/CDL plug-ins), with their closure")
    ap.add_argument("--app", choices=sorted(APP_EXTRAS), help="add the app's known run-time loads (APP_EXTRAS) to --extra")
    ap.add_argument("--cdl", action="store_true", help="add every CDL layout instance (<8 hex digits>.dll) in --dirs, with its closure")
    a = ap.parse_args()
    dirs = a.dirs.split(",") if a.dirs else [os.path.dirname(os.path.abspath(a.exe)), os.path.join(REPO, "emitter")]
    found, missing, order = {}, set(), []  # noqa
    native = set()
    # file names compare without case (a Symbian file system's rule; SIS contents are mixed case, the Linux host's is not)
    index = {d: {f.lower(): f for f in os.listdir(d)} for d in dirs if os.path.isdir(d)}

    def lookup(d, name):
        f = index.get(d, {}).get(name.lower())
        return os.path.join(d, f) if f else None
    todo = [(os.path.basename(a.exe).lower(), a.exe)]
    extra = a.extra.split(",") if a.extra else []
    if a.app:
        extra += APP_EXTRAS["common"] + APP_EXTRAS[a.app]
    if a.cdl:		# the CDL layout instances (<uid>.dll): the CDL engine loads them by UID for every Avkon app, no import names them
        extra += sorted({f for d in dirs if os.path.isdir(d) for f in os.listdir(d) if re.fullmatch(r"[0-9a-f]{8}\.dll", f)})
    for x in extra:
        for d in dirs:
            if lookup(d, x):
                todo.append((x.lower(), lookup(d, x)))
                break
        else:
            missing.add(x.lower())
    while todo:
        name, path = todo.pop()
        if name in found:
            continue
        found[name] = path
        order.append(name)
        for imp in import_names(load(path)):
            b = base(imp)
            if b in found or b in missing:
                continue
            if b in NATIVE_GL:		# the OpenGL ES / EGL DLLs run natively (proteusgl.cpp thunks their imports): an ARM copy would shadow them
                native.add(b)
                continue
            for d in dirs:
                for cand in (b, ALIAS.get(b, b)):
                    p = lookup(d, cand)
                    if p:
                        todo.append((b, p))
                        break
                else:
                    continue
                break
            else:
                missing.add(b)
    tot_te = tot_pairs = 0
    thumb_mods = []
    for name in order:
        te, ne, pairs, arm = thumb_stats(load(found[name]))
        tot_te += te
        tot_pairs += pairs
        is_thumb = te > 0 or pairs * 8 > arm		# many Thumb BL pairs relative to ARM words
        if is_thumb:
            thumb_mods.append(name)
        if not a.quiet and (te or pairs > 16):
            print(f"  {name:32} thumb exports {te:5}/{ne:<5}  thumb BL pairs {pairs:6}  ARM words {arm:7}  {'THUMB' if is_thumb else ''}  {os.path.relpath(found[name], REPO)}")
        if a.out:
            os.makedirs(a.out, exist_ok=True)
            shutil.copyfile(found[name], os.path.join(a.out, name))
    print(f"closure: {len(order)} modules ({len(order) - 1} DLLs), unresolved {len(missing)}: {', '.join(sorted(missing)) or '-'}")
    rc = 0
    if a.strict and missing - ROM_ARM:
        print("STRICT: unresolved DLLs -- build them for ARM (a stale ARM build directory has the 2010 stubs / lacks newer components)", file=sys.stderr)
        rc = 2
    if native:
        print(f"native (not staged, run on the native GL): {', '.join(sorted(native))}")
    print(f"thumb: {len(thumb_mods)} mostly-Thumb modules ({', '.join(thumb_mods) or '-'}); {tot_te} Thumb exports, {tot_pairs} Thumb BL pairs in all")
    if a.out:
        print(f"copied to {a.out}  (boot: DISKFILES={a.out}=proteus)")

    return rc


if __name__ == "__main__":
    sys.exit(main())

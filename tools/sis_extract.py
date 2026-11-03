# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""sis_extract -- the files a SIS (v9) installs, laid out for Proteus: executables under <out>/sys/bin/arm/ (where the
handler looks for ARM code before \\sys\\bin\\, so they never eclipse the native ones), every other file at its own path.

    py tools/sis_extract.py <package.sis> <out dir> [--list]

Only the package's own install block (not embedded packages, not files under a condition); '!:' = c:. Prints one line
per file.
"""
import argparse, os, struct, sys, zlib


def fields(b, o=0, e=None):
    e = len(b) if e is None else e
    while o + 8 <= e:
        t, l = struct.unpack_from("<II", b, o)
        l &= 0x7FFFFFFF
        body = b[o + 8:o + 8 + l]
        yield t, (array(body) if t == 2 else body)
        o += 8 + ((l + 3) & ~3)


def array(body):
    # SISArray = one element type + untyped elements: re-emitted as typed fields
    if len(body) < 4:
        return b""
    et, s, out = struct.unpack_from("<I", body, 0)[0], 4, b""
    while s + 4 <= len(body):
        el = struct.unpack_from("<I", body, s)[0] & 0x7FFFFFFF
        pad = (el + 3) & ~3
        out += struct.pack("<II", et, el) + body[s + 4:s + 4 + pad]
        s += 4 + pad
    return out


def compressed(body):
    alg, _ = struct.unpack_from("<IQ", body, 0)
    return zlib.decompress(body[12:]) if alg == 1 else body[12:]


def files_of(install_block):
    # SISInstallBlock = (files array, embedded SIS array, if-blocks array): the unconditional files only
    out = []
    for t, body in fields(install_block):
        if t == 2:
            for tt, fd in fields(body):
                if tt == 24:
                    fl = list(fields(fd))
                    target = fl[0][1].decode("utf-16le")
                    op, _opt, _ln, _ul, idx = struct.unpack("<IIQQI", fd[-28:])
                    out.append((target, op, idx))
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sis")
    ap.add_argument("out")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    b = open(a.sis, "rb").read()
    if struct.unpack_from("<I", b, 0)[0] != 0x10201A7A:
        sys.exit(f"{a.sis}: not a SIS v9 file")
    contents = next(body for t, body in fields(b, 16) if t == 12)
    files, data = [], None
    for t, body in fields(contents):
        if t == 3:
            for tt, ctrl in fields(compressed(body)):
                if tt == 13:
                    for ct, cb in fields(ctrl):
                        if ct == 28:
                            files = files_of(cb)
        elif t == 30:
            data = body
    units = [u for t, u in fields(next(body for t, body in fields(data) if t == 2)) if t == 31] if data else []
    # SISDataUnit = array of SISFileData, each holding one SISCompressed
    blobs = [compressed(c) for t, fd in fields(units[0]) if t == 2 for tt, fd_body in fields(fd) if tt == 32
             for ct, c in fields(fd_body) if ct == 3] if units else []
    for target, op, idx in files:
        if op != 1:			# EOpInstall only (no FN/FR/FT text, no run-on-install)
            continue
        rel = target.replace("!:", "c:").split(":", 1)[-1].lstrip("\\").replace("\\", "/")
        if rel.lower().startswith("sys/bin/"):
            rel = "sys/bin/arm/" + rel[len("sys/bin/"):]
        print(f"{idx:4} {target} -> {rel}")
        if a.list:
            continue
        dest = os.path.join(a.out, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        open(dest, "wb").write(blobs[idx])


if __name__ == "__main__":
    main()

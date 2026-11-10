#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""symabi_dump -- match every mangled name of an EABI .def file to its declaration in the 2010 headers and write the
signature and class records the native-bridge generator needs (docs: proteus-native-bridge.md "Generation").

    python3 tools/bridge/symabi_dump.py --repo <core checkout> --dll euser|efsrv [--out tools/bridge/abi]
    (in a container with clang + python3: tools/bridge/Dockerfile.tools; tools/bridge/run.sh does the docker call)

The pass is `clang -Xclang -ast-dump=json` over the ARM EABI target (armv5te, soft-float AAPCS), the reference an app
was compiled against; the JSON carries each export's return and argument classes (AAPCS-relevant: w word, p pointer,
l 64-bit integer, d double, f float, S<size>_<align> struct by value, cb function pointer, m pointer to member, v void,
c = a constructor/destructor that returns `this`), the kind (method/ctor/dtor/static/virtual/data/vtable/typeinfo/thunk)
and the vtable slot, plus a record per class (sizeof, alignof, base and field offsets, vtable slot order).
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

INT32 = {"int", "unsigned int", "long", "unsigned long", "wchar_t", "enum"}
SMALL_RET = {"bool": "u8", "char": "u8", "unsigned char": "u8", "signed char": "s8", "short": "s16",
             "unsigned short": "u16", "char16_t": "u16"}


def strip_cv(t: str) -> str:
    t = re.sub(r"\b(const|volatile|class|struct|union|typename)\b\s*", "", t or "")
    return re.sub(r"\s+", " ", t).strip()


class Classifier:
    def __init__(self, model: L.Model):
        self.m = model
        self.names: set[str] = set()
        self.structs: set[str] = set()     # record names passed/returned by value (their layouts are needed)

    def resolve(self, t: str, ctx: str | None, depth=0) -> str:
        """follow typedefs (in the class scope, then outward) down to a builtin, pointer, enum or record name"""
        t = strip_cv(t)
        if depth > 12 or not t:
            return t
        scope = (ctx or "").split("::") if ctx else []
        for i in range(len(scope), -1, -1):
            cand = "::".join(scope[:i] + [t])
            if cand in self.m.typedefs:
                return self.resolve(self.m.typedefs[cand], ctx, depth + 1)
        return t

    def classify(self, t: str | None, d: str | None, ctx: str | None, is_ret=False) -> dict:
        raw = t or ""
        ts = strip_cv(d or t or "")
        if re.search(r"\(\s*[\w:]*\s*::\s*\*", ts):
            return {"c": "m", "t": raw}
        if "(*" in ts:
            return {"c": "cb", "t": raw}
        if ts.endswith("*") or ts.endswith("&") or ts.endswith("]"):
            return {"c": "p", "t": raw}
        r = self.resolve(ts, ctx)
        if r.endswith("*") or r.endswith("&") or "(*" in r:
            return {"c": "p", "t": raw}
        if r in ("va_list", "std::__va_list", "__builtin_va_list"):
            return {"c": "w", "t": raw, "note": "va_list"}      # AAPCS va_list = a struct holding one pointer: 4 bytes, a word
        if r.startswith("TRefByValue<"):
            return {"c": "w", "t": raw, "note": "TRefByValue"}  # holds one reference
        self.names.add(raw)
        if r == "void":
            return {"c": "v", "t": raw}
        if r in ("float",):
            return {"c": "f", "t": raw}
        if r in ("double", "long double"):
            return {"c": "d", "t": raw}
        if r in ("long long", "unsigned long long"):
            return {"c": "l", "t": raw}
        if r in SMALL_RET:
            return {"c": SMALL_RET[r] if is_ret else "w", "t": raw}
        if r in INT32 or r.startswith("enum "):
            return {"c": "w", "t": raw}
        if r in self.m.enums or self._scoped(r, ctx, self.m.enums):
            return {"c": "w", "t": raw}
        rec = L.resolve_record(self.m, r, ctx or "")
        if rec:
            self.structs.add(rec)
            return {"c": "S", "t": raw, "rec": rec}
        return {"c": "S?", "t": raw, "probe": strip_cv(raw)}

    def _scoped(self, r, ctx, universe):
        scope = (ctx or "").split("::")
        return any("::".join(scope[:i] + [r]) in universe for i in range(len(scope), -1, -1))


def load_overrides(dll: str) -> dict:
    import importlib
    return importlib.import_module("overrides." + dll).MANUAL


def class_mangled(model: L.Model) -> dict[str, str]:
    return {L.mangle_name(q): q for q in model.records}


def match(defs, model: L.Model):
    by_name: dict[str, dict] = {}
    for f in model.funcs.values():
        for v in L.mangled_variants(f):
            by_name.setdefault(v, f)
    cls_by_m = class_mangled(model)
    out = []
    for ordinal, name, flags, rest in defs:
        e = {"ordinal": ordinal, "name": name}
        if "ABSENT" in flags:
            e["status"] = "absent"
        elif "DATA" in flags:
            e["status"] = "matched"
            e["kind"] = "data"
            e["size"] = int(rest[rest.index("DATA") + 1]) if len(rest) > rest.index("DATA") + 1 else None
            v = model.vars.get(name)
            if v:
                e["decl"] = v["qual"]
                e["type"] = v["type"]
            elif name.startswith(("_ZTV", "_ZTI", "_ZTS")):
                c = cls_by_m.get(name[4:])
                e["kind"] = {"V": "vtable", "I": "typeinfo", "S": "typeinfo-name"}[name[3]]
                if c:
                    e["cls"] = c
                else:
                    e["status"] = "unmatched"
            else:
                e["status"] = "unmatched"
        elif name.startswith(("_ZTV", "_ZTI", "_ZTS")):
            c = cls_by_m.get(name[4:])
            e["kind"] = {"V": "vtable", "I": "typeinfo", "S": "typeinfo-name"}[name[3]]
            e["status"] = "matched" if c else "unmatched"
            if c:
                e["cls"] = c
        elif name.startswith(("_ZTh", "_ZTv", "_ZTc")):
            e["kind"] = "thunk"
            base = re.sub(r"^_ZT(?:hn\d+_|vn?\d+_n?\d+_|c.*?_)", "_Z", name)
            f = by_name.get(base) or by_name.get(re.sub(r"^_ZT(?:hn\d+_|v\d+_n\d+_)", "_Z", name))
            e["status"] = "matched" if f else "unmatched"
            if f:
                e["decl_of"] = f["mangled"]
                e["_f"] = f
        else:
            f = by_name.get(name)
            if f:
                e["status"] = "matched"
                e["_f"] = f
            elif name in model.vars:
                e["status"] = "matched"
                e["kind"] = "data"
                e["decl"] = model.vars[name]["qual"]
            else:
                e["status"] = "unmatched"
        out.append(e)
    return out


def describe(e: dict, f: dict, cl: Classifier, slots: dict):
    ctx = f["cls"]
    e["kind"] = e.get("kind") or {"CXXConstructorDecl": "ctor", "CXXDestructorDecl": "dtor"}.get(
        f["kind"], "static" if f["static"] else ("virtual" if f["virtual"] else ("method" if f["cls"] else "function")))
    if e["kind"] == "thunk":
        e["thunk_of"] = f["kind"]
    e["decl"] = (f["cls"] + "::" if f["cls"] else "") + f["name"]
    e["cls"] = f["cls"]
    e["const"] = f["const"]
    e["virtual"] = f["virtual"] or f["kind"] == "CXXDestructorDecl" and (f["cls"] in slots and "~D1" in slots[f["cls"]])
    if e["virtual"] and f["cls"]:
        sl = slots.get(f["cls"], [])
        key = "~D1" if f["kind"] == "CXXDestructorDecl" else L.key_of(f)
        if f["kind"] == "CXXDestructorDecl" and "D0" in e["name"]:
            key = "~D0"
        if key in sl:
            e["vslot"] = sl.index(key)
    params = [cl.classify(p["t"], p["d"], ctx) for p in f["params"]]
    if f["cls"] and not f["static"]:
        params.insert(0, {"c": "p", "t": "this"})
    if f["kind"] == "CXXConstructorDecl" or (f["kind"] == "CXXDestructorDecl" and not re.search(r"D0E", e["name"])):
        ret = {"c": "c", "t": "this"}
    elif f["kind"] == "CXXDestructorDecl":
        ret = {"c": "v", "t": "void"}
    else:
        ret = cl.classify(f["ret"], None, ctx, is_ret=True)
    e["ret"], e["params"], e["variadic"] = ret, params, bool(f["variadic"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--dll", required=True, choices=sorted(L.DEFS))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "abi"))
    a = ap.parse_args()
    defs = L.read_def(os.path.join(a.repo, L.DEFS[a.dll]))
    with tempfile.TemporaryDirectory() as tmp:
        ast = L.load_ast(a.repo, "arm", a.dll, tmp)
        model = L.Model().build(ast)
        del ast
        ex = match(defs, model)
        manual = load_overrides(a.dll)
        by_m = {e["name"]: e for e in ex}
        for e in ex:
            if e["status"] == "unmatched" and e["name"] in manual:
                man = manual[e["name"]]
                if "alias" in man:
                    e["_f"] = model.funcs[man["alias"]]
                    e["alias_of"] = man["alias"]
                else:
                    e.update({k: v for k, v in man.items()})
                e["status"], e["source"] = "matched", "manual"
        cl = Classifier(model)
        memo: dict = {}
        slots = {q: L.vtable_slots(model, q, memo) for q in model.records}
        for e in ex:
            if e.get("_f"):
                keep = {k: e[k] for k in ("alias_of", "source") if k in e}
                describe(e, e["_f"], cl, slots)
                e.update(keep)
        unres = sorted({p["probe"] for e in ex for p in [e.get("ret") or {}] + (e.get("params") or []) if p.get("c") == "S?"})
        probes = L.probe_sizes(a.repo, "arm", a.dll, unres, tmp) if unres else {}
        for e in ex:
            for p in [e.get("ret") or {}] + (e.get("params") or []):
                if p.get("c") == "S?":
                    p["c"] = "S" if p["probe"] in probes else "?"
                    if p["probe"] in probes:
                        p["size"], p["align"] = probes[p["probe"]]
                        p["float"] = False
                        p["rec"] = p["probe"]
        classes = sorted({e["cls"] for e in ex if e.get("cls")} | cl.structs)
        lay = L.layouts(a.repo, "arm", a.dll, [c for c in classes if "<" not in c and c in model.records], tmp)
    recs = {}
    for c in classes:
        r = lay.get(c) or lay.get("class " + c)
        key = c
        rec = {"name": c, "slots": slots.get(c, []), "declared": c in model.records}
        if c in lay:
            rec.update({k: lay[c][k] for k in ("size", "align", "vptr", "bases", "fields") if k in lay[c]})
        elif r:
            rec.update(r)
        recs[key] = rec
    # struct-by-value float content
    for e in ex:
        for p in [e.get("ret") or {}] + (e.get("params") or []):
            if p.get("c") == "S" and "size" not in p:
                r = recs.get(p["rec"], {})
                p["size"], p["align"] = r.get("size"), r.get("align")
                p["float"] = any(re.search(r"\b(float|double|TReal\w*)\b", f["type"]) for f in r.get("fields", []))
    for e in ex:
        e.pop("_f", None)
    st = {"def": len(defs), "absent": sum(e["status"] == "absent" for e in ex),
          "matched": sum(e["status"] == "matched" for e in ex), "unmatched": [e["name"] for e in ex if e["status"] == "unmatched"],
          "classes": len(recs)}
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, a.dll + ".json"), "w", newline="\n") as f:
        json.dump({"dll": a.dll, "target": "armv5te-eabi-softfp", "stats": st, "exports": ex, "classes": recs}, f, indent=0,
                  separators=(",", ":"), sort_keys=True)
        f.write("\n")
    present = st["def"] - st["absent"]
    print(f"{a.dll}: {st['def']} ordinals, {st['absent']} ABSENT, matched {st['matched']}/{present} "
          f"({100.0 * st['matched'] / present:.2f}%), unmatched {len(st['unmatched'])}, classes {st['classes']}")


if __name__ == "__main__":
    main()

# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""symabi_lib -- shared by symabi_dump.py and layoutcheck.py: run clang over the 2010 Symbian headers for one target,
read the AST (clang -ast-dump=json) and the record layouts (-fdump-record-layouts), compute vtable slot order.

The same translation unit (umbrella_<dll>.cpp, the headers behind the DLL's .def) is compiled twice:
  arm  armv5te, soft-float AAPCS, 4-byte enums, 16-bit wchar -- what a Symbian app was compiled against (the reference)
  x32  amd64 CPU, 32-bit pointers, SysV, 4-byte enums        -- the native build (emitter/build_mmp_x32.py's line)
The defines and include paths are core's: toolchain/symcc (ARM) and tools/phorepoc-x86/port/euser_census_x32.sh (x32).
Runs inside a container that has clang and python3 (tools/bridge/Dockerfile.tools); REPO is core's checkout.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EKA = "baseline/kernelhwsrv/kernel/eka"
FS = "baseline/kernelhwsrv/userlibandfileserver/fileserver"
DEFS = {
    "euser": f"{EKA}/eabi/euseru.def",
    "efsrv": f"{FS}/eabi/efsrvu.def",
}
COMMON = ["-fsyntax-only", "-ffreestanding", "-nostdinc", "-nostdinc++", "-std=gnu++03", "-fno-exceptions", "-fno-rtti",
          "-fpermissive", "-w", "-fshort-wchar", "-fno-short-enums", "-fno-access-control", "-ferror-limit=20",
          "-D__EPOC32__", "-D__SYMBIAN32__", "-D_UNICODE", "-D__DLL__", "-DSYMBIAN_DISTINCT_LOCALE_MODEL",
          "-DSYMBIAN_ENABLE_64_BIT_FILE_SERVER_API"]
TARGETS = {
    "arm": (["-target", "armv5te-none-eabi", "-mfloat-abi=soft", "-D__GCCE__", "-D__EABI__", "-D__MARM__",
             "-D__MARM_ARMV5__", "-include", "toolchain/symbian_gcc_prefix.h",
             "-I", "toolchain/overlay", "-I", "toolchain/generated", "-I", f"{EKA}/include", "-I", f"{EKA}/euser",
             "-I", f"{EKA}/euser/unicode", "-I", f"{EKA}/common", "-I", "baseline/kernelhwsrv/halservices/hal/inc",
             "-I", "toolchain/kinc", "-I", f"{FS}/inc"]),
    "x32": (["-target", "x86_64-linux-gnux32", "-fno-pic", "-D__CPU_X86", "-D__GCC32__", "-D__NO_CLASS_CONSTS__",
             "-D__IEEE_LITTLE_ENDIAN",
             "-I", "toolchain/overlay", "-I", "tools/phorepoc-x86/port/x64user", "-I", f"{EKA}/include",
             "-I", f"{EKA}/euser", "-I", f"{EKA}/euser/unicode", "-I", f"{EKA}/common",
             "-I", "baseline/kernelhwsrv/halservices/hal/inc", "-I", "toolchain/kinc", "-I", "toolchain/generated",
             "-I", "tools/phorepoc-x86/port", "-I", f"{FS}/inc",
             "-include", "tools/phorepoc-x86/port/port_prelude.h", "-include", "e32def.h"]),
}


def clang(repo: str, target: str, dll: str, extra: list[str], src: str | None = None, out: str | None = None,
          allow_fail: bool = False) -> str:
    """run clang++ for `target` over the umbrella (or `src`); returns stderr (and writes stdout to `out`)"""
    src = src or os.path.join(HERE, f"umbrella_{dll}.cpp")
    cmd = ["clang++"] + COMMON + TARGETS[target] + ["-I", HERE] + extra + [src]
    with open(out or os.devnull, "wb") as o:
        p = subprocess.run(cmd, cwd=repo, stdout=o, stderr=subprocess.PIPE)
    if p.returncode and not allow_fail:
        raise SystemExit(f"clang ({target}) failed:\n" + p.stderr.decode(errors="replace")[:4000])
    return p.stderr.decode(errors="replace")


def read_def(path: str):
    """-> [(ordinal, name, flags(set of DATA/ABSENT/R...), data_size)]"""
    out = []
    for ln in open(path, errors="replace"):
        m = re.match(r"\s*(\S+)\s+@\s*(\d+)\s+NONAME\s*([^;]*)(;.*)?$", ln)
        if not m:
            continue
        rest = m.group(3).split()
        out.append((int(m.group(2)), m.group(1), set(rest), rest))
    return out


def load_ast(repo: str, target: str, dll: str, tmp: str):
    p = os.path.join(tmp, f"ast_{target}_{dll}.json")
    clang(repo, target, dll, ["-Xclang", "-ast-dump=json"], out=p)
    with open(p) as f:
        d = json.load(f)
    os.remove(p)
    return d


# ---- AST model ---------------------------------------------------------------------------------------------------

class Model:
    """records (qualified name -> info), functions/variables by mangled name, typedefs, enums"""

    def __init__(self):
        self.records: dict[str, dict] = {}
        self.funcs: dict[str, dict] = {}
        self.vars: dict[str, dict] = {}
        self.typedefs: dict[str, str] = {}
        self.enums: set[str] = set()
        self.by_id: dict[str, str] = {}

    def build(self, ast: dict) -> "Model":
        self._walk(ast, [], None)
        return self

    def _walk(self, n, scope, cur_rec):
        for c in n.get("inner", []):
            k = c.get("kind")
            name = c.get("name")
            if k == "NamespaceDecl":
                self._walk(c, scope + [name or ""], None)
            elif k == "LinkageSpecDecl":
                self._walk(c, scope, cur_rec)
            elif k == "CXXRecordDecl" and name:
                q = "::".join(scope + [name])
                if c.get("completeDefinition"):
                    self._record(q, c)
                    self._walk(c, scope + [name], q)
                    self._members(q, c)
            elif k == "EnumDecl" and name:
                self.enums.add("::".join(scope + [name]))
            elif k == "TypedefDecl" or k == "TypeAliasDecl":
                if name:
                    t = c.get("type", {})
                    self.typedefs["::".join(scope + [name])] = t.get("desugaredQualType") or t.get("qualType")
            elif k == "FunctionDecl" and (c.get("mangledName") or name):
                c.setdefault("mangledName", name)          # extern "C": the plain name
                self._func(c, scope, None)
            elif k == "VarDecl" and (c.get("mangledName") or name) and cur_rec is None:
                c.setdefault("mangledName", name)
                self.vars[c["mangledName"]] = {"qual": "::".join(scope + [name]), "type": c.get("type", {}).get("qualType")}

    def _record(self, q, c):
        bases = []
        for b in c.get("bases", []):
            t = b.get("type", {})
            bases.append({"name": t.get("qualType"), "virtual": bool(b.get("isVirtual"))})
        dd = c.get("definitionData", {})
        poly = bool(dd.get("isPolymorphic"))
        # non-trivial for the purposes of calls (Itanium C++ ABI 3.1.1.1): a non-trivial copy or move constructor or destructor. Such a class
        # is returned (and passed) through a hidden pointer on both sides, AAPCS and SysV, whatever its size. clang's own decision is
        # `canPassInRegisters`; the per-member `nonTrivial` flags are not it: an implicit move constructor not yet declared reads `nonTrivial`
        # (TBufBase16/8, TParse, TEntry ...), yet the x32 GCC euser/efsrv return TBuf<2> (TDriveUnit::Name) in rax:rdx
        cc, mc, dt = dd.get("copyCtor", {}), dd.get("moveCtor", {}), dd.get("dtor", {})
        if dd:		# (the JSON lists only the true flags: no key = false)
            ntcall = not dd.get("canPassInRegisters", False)
        else:
            ntcall = bool(cc.get("nonTrivial") or (mc.get("exists") and mc.get("nonTrivial")) or dt.get("nonTrivial"))
        r = self.records.setdefault(q, {"qual": q, "kind": c.get("tagUsed"), "bases": bases, "polymorphic": poly,
                                        "methods": [], "fields": []})
        r["bases"], r["polymorphic"], r["ntcall"] = bases, poly, ntcall

    def _members(self, q, c):
        r = self.records[q]
        for m in c.get("inner", []):
            k = m.get("kind")
            if k == "FieldDecl":
                r["fields"].append(m.get("name"))
            elif k in ("CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl", "CXXConversionDecl", "FunctionDecl") and m.get("mangledName"):
                self._func(m, q.split("::")[:-1] + [q.split("::")[-1]], q)
                r["methods"].append(m["mangledName"])
            elif k == "VarDecl" and m.get("mangledName"):
                self.vars[m["mangledName"]] = {"qual": q + "::" + (m.get("name") or ""), "type": m.get("type", {}).get("qualType"),
                                               "cls": q}

    def _func(self, m, scope, cls):
        params = []
        for p in m.get("inner", []):
            if p.get("kind") == "ParmVarDecl":
                t = p.get("type", {})
                params.append({"t": t.get("qualType"), "d": t.get("desugaredQualType")})
        ft = m.get("type", {}).get("qualType", "")
        ret, qual, variadic = split_functype(ft)
        self.funcs[m["mangledName"]] = {
            "mangled": m["mangledName"], "name": m.get("name"), "cls": cls, "kind": m["kind"],
            "static": m.get("storageClass") == "static", "virtual": bool(m.get("virtual")),
            "pure": bool(m.get("pure")), "const": "const" in qual, "implicit": bool(m.get("isImplicit")),
            "ret": ret, "params": params, "variadic": variadic or "..." in ft, "functype": ft,
            "ns": scope if cls is None else None,
        }


def split_functype(ft: str):
    """'TInt (TInt, const TDesC &) const' -> ('TInt', 'const', variadic)"""
    s = ft.strip()
    qual = ""
    while True:
        m = re.search(r"\)\s*((?:const|volatile|noexcept|&&|&|\s)+)$", s)
        if not m:
            break
        qual += " " + m.group(1)
        s = s[:m.start(1)].rstrip()
    if not s.endswith(")"):
        return s, qual, False
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                return s[:i].strip(), qual, s[i:].endswith("...)")
    return s, qual, False


def mangled_variants(f: dict):
    """the C1/C2 and D0/D1/D2 spellings of a constructor/destructor"""
    m = f["mangled"]
    n = (f["name"] or "").lstrip("~")
    tag = f"{len(n)}{n}"
    if f["kind"] == "CXXConstructorDecl" and tag + "C1" in m:
        return [m.replace(tag + "C1", tag + "C2", 1), m]
    if f["kind"] == "CXXDestructorDecl" and tag + "D1" in m:
        return [m.replace(tag + "D1", tag + v, 1) for v in ("D0", "D2")] + [m]
    return [m]


def mangle_name(qual: str):
    parts = qual.split("::")
    s = "".join(f"{len(p)}{p}" for p in parts)
    return s if len(parts) == 1 else f"N{s}E"


def vtable_slots(model: Model, q: str, memo: dict) -> list[str]:
    """slot order of the class's primary vtable: the primary base's slots (overridden in place) + new virtuals in
    declaration order; a destructor takes two slots (complete, deleting). Entries are 'name(params)const' keys"""
    if q in memo:
        return memo[q]
    r = model.records.get(q)
    slots: list[str] = []
    if r:
        for b in r["bases"]:
            bn = resolve_record(model, b["name"], q)
            if bn and model.records[bn]["polymorphic"] and not b["virtual"]:
                slots = list(vtable_slots(model, bn, memo))
                break
        for mg in r["methods"]:
            f = model.funcs[mg]
            if f["kind"] == "CXXDestructorDecl":
                key = "~"
            elif f["kind"] == "CXXMethodDecl" and not f["static"] and (f["virtual"] or key_of(f) in slots):
                key = key_of(f)
            else:
                continue
            if key == "~":
                if not f["virtual"] and "~D1" not in slots:
                    continue
                if "~D1" not in slots:
                    slots += ["~D1", "~D0"]
            elif key not in slots:
                slots.append(key)
    memo[q] = slots
    return slots


def key_of(f: dict) -> str:
    return f"{f['name']}({','.join(re.sub(r'\\s+', ' ', p['d'] or p['t'] or '') for p in f['params'])}){'c' if f['const'] else ''}"


def resolve_record(model: Model, name: str | None, ctx: str) -> str | None:
    if not name:
        return None
    n = re.sub(r"\b(class|struct|const|volatile)\b\s*", "", name).strip()
    scope = ctx.split("::")
    for i in range(len(scope), -1, -1):
        cand = "::".join(scope[:i] + [n])
        if cand in model.records:
            return cand
    return n if n in model.records else None


# ---- record layouts ----------------------------------------------------------------------------------------------
def force_tu(classes: list[str], dll: str, tmp: str) -> str:
    """a TU that includes the umbrella and takes sizeof() of every class (so clang lays each one out)"""
    p = os.path.join(tmp, f"force_{dll}.cpp")
    with open(p, "w") as f:
        f.write(f'#include "{os.path.join(HERE, "umbrella_" + dll + ".cpp")}"\n')
        for i, c in enumerate(classes):
            f.write(f"enum {{ forced_{i} = sizeof({c}) }};\n")
    return p


def layouts(repo: str, target: str, dll: str, classes: list[str], tmp: str) -> dict[str, dict]:
    """clang -fdump-record-layouts for the classes (and everything they pull in) -> {name: record}"""
    src = force_tu(classes, dll, tmp)
    p = os.path.join(tmp, f"lay_{target}_{dll}.txt")
    clang(repo, target, dll, ["-Xclang", "-fdump-record-layouts"], src=src, out=p)
    text = open(p, errors="replace").read()
    os.remove(p)
    return parse_layouts(text)


def parse_layouts(text: str) -> dict[str, dict]:
    out: dict[str, dict] = {}
    cur = None
    for ln in text.splitlines():
        if ln.startswith("*** Dumping AST Record Layout"):
            cur = None
            continue
        m = re.match(r"\s*(\d+)(?::(\d+)-(\d+))?\s*\|(\s*)(.*)$", ln)
        if m:
            off, depth, body = int(m.group(1)), len(m.group(4)), m.group(5).strip()
            if cur is None:
                mm = re.match(r"(class|struct|union)\s+(.*)$", body)
                if mm:
                    cur = {"kind": mm.group(1), "name": re.sub(r" \(empty\)$", "", mm.group(2)), "bases": [], "fields": [], "vptr": False}
                    cur["_d0"] = depth
                    out[cur["name"]] = cur
                continue
            if depth == cur["_d0"] + 2:
                if body.startswith("(") and "vtable pointer" in body:
                    cur["vptr"] = True
                    cur["vptr_off"] = off
                elif body.startswith("(") and "vbtable" in body:
                    cur["vbptr"] = off
                else:
                    mb = re.match(r"(?:virtual\s+)?(?:class|struct)\s+(.*?)\s*\((?:primary |virtual )?base\)$", body)
                    if mb:
                        cur["bases"].append({"name": mb.group(1), "off": off, "virtual": body.startswith("virtual")})
                    elif m.group(2):
                        cur["fields"].append({"type": body, "name": "<bitfield>", "off": off, "bits": f"{m.group(2)}-{m.group(3)}"})
                    else:
                        mf = re.match(r"(?:(?:class|struct|union)\s+)?(.*?)\s*(\w+(?:\[\d*\])*)$", body)
                        if mf:
                            cur["fields"].append({"type": mf.group(1), "name": mf.group(2), "off": off})
            continue
        m = re.search(r"\[sizeof=(\d+), (?:dsize=\d+, )?align=(\d+)", ln)
        if m and cur is not None:
            cur["size"], cur["align"] = int(m.group(1)), int(m.group(2))
            continue
        m = re.search(r"nvsize=(\d+), nvalign=(\d+)", ln)
        if m and cur is not None:
            cur["nvsize"], cur["nvalign"] = int(m.group(1)), int(m.group(2))
    for r in out.values():
        r.pop("_d0", None)
    return out


def probe_sizes(repo: str, target: str, dll: str, types: list[str], tmp: str) -> dict[str, tuple[int, int]]:
    """sizeof and alignof of arbitrary type spellings (template instances, typedefs), read from clang's diagnostics"""
    p = os.path.join(tmp, f"probe_{dll}.cpp")
    with open(p, "w") as f:
        f.write(f'#include "{os.path.join(HERE, "umbrella_" + dll + ".cpp")}"\ntemplate<unsigned long long N> struct Z;\n')
        for i, t in enumerate(types):
            f.write(f"Z<sizeof({t})> zs_{i}; Z<__alignof__({t})> za_{i};\n")
    err = clang(repo, target, dll, ["-ferror-limit=0"], src=p, allow_fail=True)
    got = re.findall(r"undefined template 'Z<(\d+)>'", err)
    out = {}
    for i, t in enumerate(types):
        if 2 * i + 1 < len(got):
            out[t] = (int(got[2 * i]), int(got[2 * i + 1]))
    return out

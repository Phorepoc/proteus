#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""gen_bridge -- the ARM -> native-x32 call tables for the system libraries (approach (b), docs: proteus-native-bridge.md).
The generalisation of gen_gl_thunks.py: there the signatures come from the Khronos headers; here from
tools/bridge/abi/<dll>.json (tools/bridge/symabi_dump.py over the 2010 headers and the EABI .def).

    python3 tools/gen_bridge.py [euser efsrv]
    -> os/src/proteus/gen/bridge_shapes.inc   one trampoline per distinct call shape (AAPCS soft-float -> SysV x32)
       os/src/proteus/gen/bridge_<dll>.inc    one row per ordinal: { ordinal, mangled name, shape trampoline, flags, vslot },
                                              the class sizes as static_asserts (BRIDGE_LAYOUT_ASSERTS), the overrides
Nothing includes these yet (R0: tooling only); tests/bridge/bridge_inc_test.cpp compiles them.

A shape is `<ret>:<args>` with the codes  w word (int, enum, pointer, reference, callback)  l 64-bit integer  d double
f float  S<size>x<align> a struct by value  and for returns also v void, b/B 8-bit unsigned/signed, h/H 16-bit, c = a
constructor/destructor that returns `this` (ARM) where native returns void.  The trampoline reads the arguments the way
the AAPCS passes them (a 64-bit value takes an even register pair or an 8-aligned stack slot, softfp keeps floats in core
registers), calls the native function and stores the result in r0 (r0:r1 for 64 bits, a hidden pointer in r0 for a struct
larger than 4 bytes).  Exports that need a hand-written handler (varargs, structs holding floats, unresolved types) get no
trampoline and a flag; tools/bridge/overrides/<dll>.py names the handlers.
"""
from __future__ import annotations

import importlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "bridge"))
ABI = os.path.join(HERE, "bridge", "abi")
GEN = os.path.join(HERE, "..", "os", "src", "proteus", "gen")

FLAGS = ["VARIADIC", "CALLBACK", "STRUCTFP", "UNRESOLVED", "DATA", "VTABLE", "TYPEINFO", "THUNK", "VIRTUAL", "ABSENT",
         "CTOR", "MANUAL", "PMF", "OVERRIDE"]
RETCODE = {"v": "void", "w": "uint32_t", "p": "uint32_t", "b": "uint8_t", "B": "int8_t", "h": "uint16_t", "H": "int16_t",
           "f": "float", "d": "double", "l": "uint64_t"}
RET_FROM_CLASS = {"u8": "b", "s8": "B", "u16": "h", "s16": "H"}


# structs whose function-pointer field is a typedef the symabi dump's "(*" test misses (TCleanupOperation, TInt (*)(TAny*) via a typedef)
KNOWN_FP = {"TCleanupItem": [0], "TCallBack": [0]}


def arg_code(p: dict) -> str:
    c = p["c"]
    if c in ("w", "p"):
        return "w"
    if c == "cb":
        return "k"			# a function pointer: a guest function becomes a reverse stub (BrCb)
    if c in ("l", "d", "f"):
        return c
    if c == "S":
        fp = p.get("fp") or KNOWN_FP.get(p.get("rec"), [])
        return f"S{p['size']}x{p['align']}" + ("f" + ".".join(map(str, fp)) if fp else "")
    return "?"


def shape_of(e: dict) -> tuple[str | None, int]:
    """-> (shape string or None when a hand-written handler is needed, flags)"""
    fl = 0
    ps = e.get("params", [])
    r = e["ret"]
    if e.get("variadic") or any(p.get("note") == "va_list" for p in ps):     # a va_list is an AAPCS struct: the native walker reads it
        fl |= 1 << FLAGS.index("VARIADIC")
    if any(p["c"] == "cb" for p in ps):
        fl |= 1 << FLAGS.index("CALLBACK")
    if any(p["c"] == "m" for p in ps + [r]):
        fl |= 1 << FLAGS.index("PMF")
    if any(p["c"] == "S" and p.get("float") for p in ps + [r]):
        fl |= 1 << FLAGS.index("STRUCTFP")
    if any(p["c"] in ("?", "m") for p in ps + [r]):
        fl |= 1 << FLAGS.index("UNRESOLVED")
    if fl & ((1 << FLAGS.index("VARIADIC")) | (1 << FLAGS.index("STRUCTFP")) | (1 << FLAGS.index("UNRESOLVED")) |
             (1 << FLAGS.index("PMF"))):
        return None, fl
    rc = r["c"]
    if rc == "S":
        rcode = f"S{r['size']}x{r['align']}"		# (a returned struct is never rewritten: no function pointer comes back from native)
    elif rc in RET_FROM_CLASS:
        rcode = RET_FROM_CLASS[rc]
    elif rc == "cb":
        rcode = "w"
    else:
        rcode = rc
    return rcode + ":" + "_".join(arg_code(p) for p in ps), fl


def struct_name(code: str) -> str:
    m = re.match(r"S([0-9]+)x([0-9]+)", code)
    return f"BrS{m.group(1)}A{m.group(2)}"


def emit_shape(idx: int, shape: str) -> list[str]:
    rcode, args = shape.split(":")
    atoks = [a for a in args.split("_") if a]
    L = [f"// {shape}", f"static void BrShape_{idx}(CArmCpu& aCpu, uintptr_t aFn)", "{"]
    big_ret = rcode.startswith("S") and int(re.match(r"S(\d+)", rcode).group(1)) > 4
    L.append("\tBrArgs a(aCpu);")
    if big_ret:
        L.append("\tconst uint32_t ret = a.W();")
    names = []
    ctypes = []
    for i, t in enumerate(atoks):
        n = f"a{i}"
        names.append(n)
        if t == "w":
            ctypes.append("uint32_t")
            L.append(f"\tconst uint32_t {n} = a.W();")
        elif t == "k":
            ctypes.append("uint32_t")
            L.append(f"	const uint32_t {n} = BrCb(aCpu, a.W());")
        elif t == "l":
            ctypes.append("uint64_t")
            L.append(f"\tconst uint64_t {n} = a.L();")
        elif t == "d":
            ctypes.append("double")
            L.append(f"\tconst double {n} = a.D();")
        elif t == "f":
            ctypes.append("float")
            L.append(f"\tconst float {n} = a.F();")
        else:
            sn = struct_name(t)
            ctypes.append(sn)
            L.append(f"\t{sn} {n}; a.S(&{n}, sizeof({sn}), __alignof__({sn}));")
            mf = re.search(r"f([0-9.]+)$", t)
            for off in (mf.group(1).split(".") if mf else []):
                L.append(f"\tBrCbField(aCpu, (uint8_t*)&{n} + {off});\t// a function pointer inside the struct")
    if rcode == "c":
        rt = "void"
    elif rcode.startswith("S"):
        rt = struct_name(rcode)
    else:
        rt = RETCODE[rcode]
    L.append(f"\ttypedef {rt} (*F)({', '.join(ctypes) or 'void'});")
    call = f"((F)(uintptr_t)aFn)({', '.join(names)})"
    if rcode == "v":
        L.append(f"\t{call};")
    elif rcode == "c":
        L.append(f"\t{call};")
        L.append("\taCpu.R[0] = a0;\t\t\t// ARM constructors and non-deleting destructors return `this`")
    elif rcode in ("w", "p"):
        L.append(f"\taCpu.R[0] = {call};")
    elif rcode in ("b", "B", "h", "H"):
        L.append(f"\taCpu.R[0] = (uint32_t){call};\t\t\t// the native side leaves the upper bits undefined")
    elif rcode == "f":
        L.append(f"\tconst float r = {call};\n\tmemcpy(&aCpu.R[0], &r, 4);")
    elif rcode == "d":
        L.append(f"\tconst double r = {call};\n\tmemcpy(&aCpu.R[0], &r, 8);")
    elif rcode == "l":
        L.append(f"\tconst uint64_t r = {call};\n\taCpu.R[0] = (uint32_t)r;\n\taCpu.R[1] = (uint32_t)(r >> 32);")
    else:
        L.append(f"\tconst {rt} r = {call};")
        if big_ret:
            L.append("\tmemcpy(aCpu.HostPtr(ret), &r, sizeof(r));\n\taCpu.R[0] = ret;")
        else:
            L.append("\taCpu.R[0] = 0;\n\tmemcpy(&aCpu.R[0], &r, sizeof(r));")
    L.append("}")
    return L


def vt_args(abi, abis):
    """(export name, AAPCS word index, slots) for every pointer/reference argument (not `this`) of a class with a vtable."""
    classes = {}
    for a in abis.values():
        for c, r in a["classes"].items():
            if r.get("slots"):
                classes.setdefault(c, len(r["slots"]))
    out = []
    for e in abi["exports"]:
        if e.get("status") == "absent" or "params" not in e or e.get("variadic"):
            continue
        word = 0
        for p in e["params"]:
            c, t = p["c"], p.get("t", "")
            if c in ("l", "d"):
                word += (word & 1) + 2
                continue
            if c == "S":
                break                                   # (a by-value struct: its size is not here; later arguments are not tracked)
            if c == "p" and t != "this":
                base = re.sub(r"^const ", "", t).strip()
                if re.fullmatch(r"[A-Za-z_][\w:]* *[*&]", base):
                    cls = base.rstrip("*& ").strip()
                    if cls in classes:
                        out.append((e["name"], word, classes[cls]))
                    elif cls in KNOWN_FP:		# a TCallBack / TCleanupItem by reference: slots = -1 - the field offset (bridge.cpp)
                        out.append((e["name"], word, -1 - KNOWN_FP[cls][0]))
            word += 1
    return out


def mangle_cls(c: str) -> str | None:
    """The Itanium source-name of a plain class (no templates, no namespaces): `7CActive`."""
    return f"{len(c)}{c}" if re.fullmatch(r"[A-Za-z_]\w*", c) else None


def ti_table(abi):
    """ARM type_info objects the bridge synthesizes for the DLL's `_ZTI*` DATA exports (the native build is -fno-rtti; the guest's own
    drtaeabi runs dynamic_cast and the catch matching): (export name or "" for a base that is not exported, mangled name, base index or -1).
    Single non-virtual bases at offset 0 only (every euser/efsrv case); a base that cannot be named (a template) is left out."""
    classes = abi["classes"]
    out, index = [], {}

    def add(cls, export):
        m = mangle_cls(cls)
        if m is None:
            return -1
        if m in index:
            if export and not out[index[m]][0]:
                out[index[m]] = (export, m, out[index[m]][2])
            return index[m]
        index[m] = len(out)
        out.append((export, m, -1))
        bases = (classes.get(cls) or {}).get("bases") or []
        if len(bases) == 1 and not bases[0].get("virtual") and bases[0].get("off", 0) == 0:
            b = add(bases[0]["name"], "")
            out[index[m]] = (out[index[m]][0], m, b)
        return index[m]

    for e in abi["exports"]:
        if e.get("kind") == "typeinfo" and e["name"].startswith("_ZTI"):
            m = e["name"][4:]
            if re.fullmatch(r"\d+[A-Za-z_]\w*", m):
                add(e["cls"], e["name"])
            else:		# a template instance: no base information, a plain __class_type_info with the export's own name
                index[m] = len(out)
                out.append((e["name"], m, -1))
    return out


def main():
    dlls = sys.argv[1:] or ["euser", "efsrv"]
    abis = {d: json.load(open(os.path.join(ABI, d + ".json"))) for d in dlls}
    native = {}
    for d in dlls:
        np_ = os.path.join(HERE, "bridge", "native", d + ".json")
        native[d] = json.load(open(np_)) if os.path.exists(np_) else {}
    shapes: dict[str, int] = {}
    rows: dict[str, list] = {}
    stats: dict[str, dict] = {}
    for d, abi in abis.items():
        ov = importlib.import_module("overrides." + d).OVERRIDES
        rows[d] = []
        st = stats[d] = {"ordinals": 0, "absent": 0, "shaped": 0, "data": 0, "override": 0, "variadic": 0, "structfp": 0,
                         "unresolved": 0, "callback": 0, "manual": 0, "ptr_statics": 0}
        for e in abi["exports"]:
            st["ordinals"] += 1
            fl, shape, slot = 0, None, -1
            kind = e.get("kind")
            if e["status"] == "absent":
                fl |= 1 << FLAGS.index("ABSENT")
                st["absent"] += 1
            elif kind in ("data", "vtable", "typeinfo", "typeinfo-name"):
                fl |= 1 << FLAGS.index({"data": "DATA", "vtable": "VTABLE", "typeinfo": "TYPEINFO", "typeinfo-name": "TYPEINFO"}[kind])
                st["data"] += 1
            elif e.get("ret"):
                shape, fl2 = shape_of(e)
                fl |= fl2
                if shape is not None and shape not in shapes:
                    shapes[shape] = len(shapes)
                st["shaped"] += shape is not None
                st["variadic"] += bool(e.get("variadic"))
                st["structfp"] += bool(fl2 & (1 << FLAGS.index("STRUCTFP")))
                st["unresolved"] += bool(fl2 & (1 << FLAGS.index("UNRESOLVED")))
                st["callback"] += bool(fl2 & (1 << FLAGS.index("CALLBACK")))
                st["ptr_statics"] += kind == "static" and e["ret"]["c"] == "p"
            if kind == "thunk":
                fl |= 1 << FLAGS.index("THUNK")
            if e.get("virtual"):
                fl |= 1 << FLAGS.index("VIRTUAL")
                slot = e.get("vslot", -1)
            if kind in ("ctor", "dtor"):
                fl |= 1 << FLAGS.index("CTOR")
            if e.get("source") == "manual":
                fl |= 1 << FLAGS.index("MANUAL")
                st["manual"] += 1
            handler = ov.get(e["name"])
            if handler:
                fl |= 1 << FLAGS.index("OVERRIDE")
            needs = shape is None and not (fl & ((1 << FLAGS.index("ABSENT")) | (1 << FLAGS.index("DATA")) |
                                                 (1 << FLAGS.index("VTABLE")) | (1 << FLAGS.index("TYPEINFO"))))
            st["override"] += needs and not handler
            st["handled"] = st.get("handled", 0) + (bool(handler) and needs)
            rows[d].append((e["ordinal"], e["name"], shape, fl, slot, handler))
    os.makedirs(GEN, exist_ok=True)
    # ---- shapes
    out = ["// Copyright The Phorepoc Authors", "// SPDX-License-Identifier: Apache-2.0",
           "// GENERATED by tools/gen_bridge.py -- do not edit. One ARM (AAPCS soft-float) -> native (SysV x32) trampoline per call shape.",
           "// Needs: CArmCpu (R[16]), BrArgs (W/L/D/F/S: the AAPCS argument cursor), <string.h>, <stdint.h>.", "",
           "enum TBridgeFlag"]
    out.append("{")
    out += [f"\tBR_{n} = 1 << {i}," for i, n in enumerate(FLAGS)]
    out.append("};")
    out += ["", "struct TBridgeExport", "{", "\tuint16_t iOrdinal;", "\tconst char* iName;",
            "\tvoid (*iCall)(CArmCpu& aCpu, uintptr_t aFn);\t// 0 = data, absent, or needs a handler (see iFlags)",
            "\tuint32_t iFlags;", "\tint16_t iVSlot;\t\t\t// the vtable slot of an exported virtual, else -1", "	uint16_t iNative;		// the ordinal of the same NAME in the native x32 DLL (tools/bridge/native/<dll>.json), 0 = none", "};", "",
            "struct TBridgeVtArg { const char* iName; int iWord; int iSlots; };	// an argument of a polymorphic class type (bridge.cpp PatchVtable)",
            "struct TBridgeTi { const char* iName; const char* iMangled; int iBase; };	// an ARM type_info the bridge synthesizes (bridge.cpp BridgeTypeInfo)", ""]
    structs = sorted({t for s in shapes for t in re.findall(r"S\d+x\d+", s)})
    for t in structs:
        n, al = map(int, re.match(r"S(\d+)x(\d+)", t).groups())
        out.append(f"struct {struct_name(t)} {{ uint8_t b[{n}] __attribute__((aligned({al}))); }};\t// a {n}-byte struct, alignment {al}")
    out.append("")
    for s, i in sorted(shapes.items(), key=lambda kv: kv[1]):
        out += emit_shape(i, s)
    out += ["", f"static const uint32_t KBridgeShapeCount = {len(shapes)};", ""]
    open(os.path.join(GEN, "bridge_shapes.inc"), "w", newline="\n").write("\n".join(out) + "\n")
    # ---- per DLL
    for d, abi in abis.items():
        L = ["// Copyright The Phorepoc Authors", "// SPDX-License-Identifier: Apache-2.0",
             f"// GENERATED by tools/gen_bridge.py from tools/bridge/abi/{d}.json -- do not edit. The {d} ordinals of the EABI .def.", ""]
        for h in sorted({r[5] for r in rows[d] if r[5]}):
            L.append(f"void {h}(CArmCpu& aCpu, uintptr_t aFn);\t// hand-written (tools/bridge/overrides/{d}.py)")
        L.append(f"static const TBridgeExport KBridge_{d}[] =")
        L.append("{")
        for ordn, name, shape, fl, slot, handler in rows[d]:
            fn = handler or (f"BrShape_{shapes[shape]}" if shape else "0")
            L.append(f'\t{{ {ordn}, "{name}", {fn}, {fl}, {slot}, {native[d].get(name, 0)} }},')
        L.append("};")
        L.append(f"static const uint32_t KBridge_{d}_Count = sizeof(KBridge_{d}) / sizeof(KBridge_{d}[0]);")
        # the pointer/reference arguments of a polymorphic class type: an ARM-derived object handed to native code, whose base-class vtable
        # slots the bridge patches to reverse stubs (bridge.cpp PatchVtable). {export, AAPCS word index, slot count of the declared class}
        vt = vt_args(abis[d], abis)
        L.append(f"static const TBridgeVtArg KBridgeVt_{d}[] =")
        L.append("{")
        for name, word, n in vt:
            L.append(f'	{{ "{name}", {word}, {n} }},')
        L.append('	{ 0, 0, 0 }')
        L.append("};")
        stats[d]["vtargs"] = len(vt)
        # ARM type_info objects for the `_ZTI*` imports (bridge.cpp BridgeTypeInfo): {export or "", mangled name, base index or -1}
        ti = ti_table(abis[d])
        L.append(f"static const TBridgeTi KBridgeTi_{d}[] =")
        L.append("{")
        for name, m, b in ti:
            L.append(f'	{{ "{name}", "{m}", {b} }},')
        L.append('	{ 0, 0, -1 }')
        L.append("};")
        stats[d]["typeinfo"] = len(ti)
        L.append("")
        L.append("#ifdef BRIDGE_LAYOUT_ASSERTS\t\t// the headers are included: the native layout equals the ARM one the app compiled in")
        for c, r in sorted(abi["classes"].items()):
            if "size" in r and "<" not in c and r.get("declared", True):
                L.append(f"typedef char BrLayout_{re.sub(r'\W', '_', c)}[(sizeof({c}) == {r['size']} && __alignof__({c}) == {r['align']}) ? 1 : -1];")
        L.append("#endif")
        open(os.path.join(GEN, f"bridge_{d}.inc"), "w", newline="\n").write("\n".join(L) + "\n")
    print(f"bridge_shapes.inc: {len(shapes)} distinct shapes ({len(structs)} by-value struct types)")
    for d, st in stats.items():
        print(f"bridge_{d}.inc: " + ", ".join(f"{k}={v}" for k, v in st.items()))
    print("overrides needed (no trampoline): " + str(sum(st["override"] for st in stats.values())))


if __name__ == "__main__":
    main()

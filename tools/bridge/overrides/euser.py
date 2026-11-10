# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""Hand-written declarations for euser exports the 2010 headers do not (or no longer) declare, and bridge overrides.

MANUAL: mangled name -> record fields (kind, cls, ret, params as AAPCS classes; `alias` = the descriptor of another
mangled name). OVERRIDES: mangled name -> name of a hand-written C++ handler (none yet: R1 adds them for the varargs)."""
W, P, V, C = {"c": "w"}, {"c": "p"}, {"c": "v"}, {"c": "c"}
THIS = {"c": "p", "t": "this"}
MANUAL = {
    # explicit instantiations in the .def (the headers only have the template)
    **{f"_ZN13CArrayFixFlatI{t}EC{n}Ei": {"kind": "ctor", "cls": f"CArrayFixFlat<{r}>", "ret": C, "params": [THIS, W]}
       for t, r in (("4TUid", "TUid"), ("i", "TInt")) for n in (1, 2)},
    **{f"_ZN13CArrayFixFlatI{t}ED{n}Ev": {"kind": "dtor", "cls": f"CArrayFixFlat<{r}>", "ret": V if n == 0 else C, "params": [THIS]}
       for t, r in (("4TUid", "TUid"), ("i", "TInt")) for n in (0, 1, 2)},
    **{f"_ZT{k}13CArrayFixFlatI{t}E": {"kind": {"V": "vtable", "I": "typeinfo"}[k], "cls": f"CArrayFixFlat<{r}>"}
       for t, r in (("4TUid", "TUid"), ("i", "TInt")) for k in "VI"},
    # legacy spelling of Mem::Copy/Fill/Move (a class "Mem1" the headers never had): the same functions
    "_ZN4Mem14CopyEPvPKvi": {"alias": "_ZN3Mem4CopyEPvPKvi"},
    "_ZN4Mem14FillEPvi5TChar": {"alias": "_ZN3Mem4FillEPvi5TChar"},
    "_ZN4Mem14MoveEPvPKvi": {"alias": "_ZN3Mem4MoveEPvPKvi"},
    # declared only under __LEAVE_EQUALS_THROW__ / the emulator build
    "_ZNK15XLeaveException9GetReasonEv": {"kind": "method", "cls": "XLeaveException", "ret": W, "params": [THIS], "const": True},
    "_ZN4Exec19SetWin32RuntimeHookEPv": {"kind": "function", "cls": None, "ret": V, "params": [P]},
    # data defined in a .cpp, never declared in a header
    "KHeapMinCellSize": {"kind": "data", "size": 4},
    "KHeapShrinkHysRatio": {"kind": "data", "size": 4},
}
OVERRIDES: dict[str, str] = {}

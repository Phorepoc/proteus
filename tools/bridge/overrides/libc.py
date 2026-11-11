# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""Hand-written declarations and handlers for Open C's libc (rung R1 round 16; see euser.py).

The printf / scanf families (`...` and the v-functions taking an ARM va_list) go through one format walker (bridge.cpp LibcFormatCall); the
calls with one optional word (open, fcntl, ioctl, semctl) pass it always; setjmp / longjmp work on the guest's registers; exit ends the guest
as User::Exit does."""
W, P, V = {"c": "w"}, {"c": "p"}, {"c": "v"}
MANUAL = {
    # __crt0(int& argc, char**& argv, char**& envp) / the wide twin: the native one fills them (its memory is the guest's too)
    "_Z6__crt0RiRPPcS2_": {"kind": "function", "ret": V, "params": [P, P, P]},
    "_Z6__crt0RiRPPwS2_": {"kind": "function", "ret": V, "params": [P, P, P]},
    "do_abort": {"kind": "function", "ret": V, "params": []},
}
_FMT = ["printf", "fprintf", "sprintf", "snprintf", "asprintf", "scanf", "fscanf", "sscanf", "wprintf", "fwprintf", "swprintf", "wscanf",
        "fwscanf", "swscanf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "vasprintf", "vscanf", "vfscanf", "__vfscanf", "vsscanf",
        "vwprintf", "vfwprintf", "vswprintf", "vwscanf", "vfwscanf", "vswscanf"]
OVERRIDES: dict[str, str] = {n: "BrOvr_libc_" + n for n in _FMT}
OVERRIDES.update({
    "open": "BrOvr_libc_Words3", "wopen": "BrOvr_libc_Words3", "fcntl": "BrOvr_libc_Words3", "ioctl": "BrOvr_libc_Words3",
    "semctl": "BrOvr_libc_Words4",
    "exit": "BrOvr_libc_exit", "_exit": "BrOvr_libc__exit", "_Exit": "BrOvr_libc__exit", "abort": "BrOvr_libc_abort",
    "setjmp": "BrOvr_libc_setjmp", "longjmp": "BrOvr_libc_longjmp",
})
# strfmon (doubles by `%n` / `%i`) stays without a handler: the bridge stops with a note if a guest calls it

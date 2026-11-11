<!-- Copyright The Phorepoc Authors -->
<!-- SPDX-License-Identifier: Apache-2.0 -->
# tools/bridge -- the signature and layout tooling of the native system-library bridge (rung R0)

Plan: the private repo's `docs/phorepoc/research/proteus-native-bridge.md`. Everything runs in the clang container
(`docker build -t proteus-bridge-tools -f tools/bridge/Dockerfile.tools tools/bridge`) against core's checkout (the 2010 headers, the
EABI `.def` files, the x32 build's defines):

| tool | what |
|---|---|
| `run.sh <core> <script.py> [args]` | runs one of the scripts below in the container |
| `symabi_dump.py --dll euser\|efsrv\|libc\|libpthread` | clang `-ast-dump=json` over `umbrella_<dll>.cpp` (ARM EABI target); matches every `.def` name to its declaration; writes `abi/<dll>.json` (checked in) |
| `layoutcheck.py` | every class with exported members laid out for ARM and for x32; diffs sizeof/alignof/bases/fields/vtable order |
| `namecheck.py` | EABI names the native x32 euser/efsrv do not export (binding is by name) |
| `overrides/<dll>.py` | hand-written declarations (`MANUAL`) and handler names (`OVERRIDES`) |
| `../gen_bridge.py` | `abi/*.json` -> `os/src/proteus/gen/bridge_{shapes,euser,efsrv}.inc` |
| `../../tests/bridge/run.sh <core>` | compiles the tables, runs layoutcheck and the layout asserts |

Open C (`libc`, `libpthread`, round 16) is read from the exported stdapis tree (`.epoc32/include/stdapis` of the checkout, as
the build exports it); `native/<dll>.json` is the .def itself there: the x32 build exports Open C by the same EABI .def.

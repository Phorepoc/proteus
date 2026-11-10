# proteus

Proteus runs ARM Symbian programs on the PC edition of [Phorepoc OS](https://github.com/Phorepoc). An app installed from
an ARM SIS starts like any other: core's loader sees an EXE built for another CPU and hands it to its handler,
`cpuhandler_arm.exe`. The handler interprets the ARM code in user space, and the program's own SecureId and declared
capabilities apply to it.

It's a layer: add it to a board's `PHOREPOC_ADAPTATIONS` and every ROM built for that board carries the handler. Core
has only the generic foreign-CPU hook and never names Proteus.

## What's here

- `os/src/proteus/`: the handler. It contains the ARM and Thumb interpreter (`armcpu.cpp`, `thumb.cpp`), the E32Image
  loader for ARM images (`proteusrt.cpp`) and the executive calls of the real ARM `euser.dll` (`proteusexec.cpp`). It
  also holds the IPC bridge, which forwards the guest's sessions to native servers.
- `os/src/proteusimg/`: the image broker. It opens ARM images under `\sys\bin\` for a handler that doesn't hold
  `AllFiles`.
- `os/src/proteus*/`: the ARM test programs behind the boot test's phases.
- `tests/`: the boot test (`board_boot.py`, phases P0 to P3), the instruction differential test (`tests/isa`) and a
  host build of the handler (`tests/proteus`).
- `tools/`: the ARM closure an app needs (`proteus_closure.py`), the ordinal check, and E32Image compression.

## Building

Build it like any other layer, with `/w/proteus` in `PHOREPOC_ADAPTATIONS`. The handler is an x32 component; the test
programs are ARM (`ROM_ARCH=arm phe component build -arch arm <name>`).

## Installing it on a PC image

A PC image built without this layer gets Proteus from the Catalogue, or from `proteus.sis` of a
[release](https://github.com/Phorepoc/proteus/releases), through the normal installer. The package holds the handler
`\sys\bin\cpuhandler_arm.exe` (with the dynarmic JIT; core's loader finds it on any drive), the image broker
`\sys\bin\proteusimg.exe` and the licence texts in `\resource\proteus\`. The handler declares `All -TCB` and the broker
`AllFiles ProtServ`: Phorepoc has no app signing, and the install server's developer mode (`PhorepocDevMode` in core's
`swipolicy.ini`) grants a package the capabilities it declares, so the unsigned package installs as it is. No ARM system
DLLs ship in it: see `docs/compat.md` "Installing Proteus" for what an app still needs.

The release Action (`.github/workflows/release.yml`) builds the SIS with the pdk's `package.yml` (`package.conf`,
`recipes/prebuild.sh` for cxxrt + dynarmic, `recipes/sis.sh`) and attaches it to a release.

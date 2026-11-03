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

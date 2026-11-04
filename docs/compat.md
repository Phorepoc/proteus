# Proteus application compatibility

Which real ARM Symbian applications run under Proteus on the PC image, how far each gets, what stops it, and what was fixed. One row per
application; newest first in each section. Method: the P3 flow (`c:\sis\*.sis` installed by the real installer, or an EXE placed in
`c:\sys\bin\`, plus `c:\sisapp.txt` naming the EXE, started the way the menu starts an app), PC image ISO with `SSM_TEST=proteus`, VirtualBox
(2 CPUs, 1 GB, VMSVGA, AC97 on the null audio driver), serial log + `VBoxManage controlvm screenshotpng`. P3 PASS = still running after 240 s
and a window group of its own in the native WSERV. The ARM system DLLs an app imports come from the closure tool
(`tools/proteus_closure.py`, from the ARM build) staged in `c:\sys\bin\arm\`.

## Table (2026-11-04, proteus after "module table" commit, VirtualBox)

| App | Type | Result | Blocker | Fix |
|---|---|---|---|---|
| Sparkle (Qt + OpenGL ES 1) | Qt 4.7 game | P0-P3 PASS, playable, **music plays** (the MMF controller is hosted natively: `sparkle-gl-run.md` "Music: the MMF controller runs natively") (see `sparkle-gl-run.md`); regression re-run on this build PASS, `img/compat-sparkle-vbox.png` | -- | -- |
| Soundwave (Qt Quick / QML) | Qt 4.7 + QtMobility app | P3 **PASS** (running, window group `200409a3`). Starts Qt, loads its QML, then stops at a white page: `module "QtMultimediaKit" is not installed` (`img/compat-soundwave-vbox.png`) | (1) **fixed**: the ARM module table held 256 modules; Soundwave's closure (Qt + CDL/ECom plugins) needs more, the 257th `RLibrary::Load` (`101fe2aa.dll`, a CDL plugin) answered -4 and `qt_init` failed ("Eikon application construct failed (-4)"). (2) **open, not Proteus**: the app imports the QML module `QtMultimediaKit`; the Qt Mobility package staged on the disk (`00b-qtmobility.sis`) installs it as `\resource\Qt\imports\QtMobility\multimedia`; copying it under `QtMultimediaKit\` (qmldir + `declarative_multimedia.qtplugin`) did not make Qt find it. The Qt Mobility build the app was made for (1.2, `import QtMultimediaKit 1.1`) is not the one on the disk | (1) `KMaxModules` 256 -> 512 (`proteusrt.h`; the import slot encoding allows 1023). (2) needs the matching Qt Mobility SIS |
| LetsGolf2 (Gameloft, native C++ + GLES) | Avkon (non-Qt) game, EXE only (`extracted/games/letsgolf2`: the EXE and `Logo.mp4`, no SIS, no game data) | FAIL: exits with -25 (KErrEof) after 16.7 M instructions, right after apparc's command line is read | (1) **fixed**: `libstdcpp.dll` (Open C++ STL, UID 10282872, imported alongside `libstdcppv5`) was not in the ARM set, so the first call into it halted ("ordinal 992 not emulated") -- built it from the 2010 source (`phe component build -arch arm libstdcpp`, already in core's registry) and added it to the closure. (2) **open**: the game's data (`Gameloft\Games\LetsGolf2\data\*.pak`, sounds, the `*_reg.rsc` application registration) is not in the repo; the EXE's own exit on a missing data file is the likely cause but the exiting code was not located. No DRM/licence check was looked at or touched | (1) built + staged. New diagnostic: `User::Exit(reason)` from the guest now logs the guest backtrace (the stack scan found no caller frame for this exit, so it did not locate it) |
| Calculator `calcsoft.exe` from the real 701 ROM (copied as `pcalc.exe`) | plain Avkon, Thumb, real Belle binary | FAIL at load: `ordinal 4611 not exported by avkon[100056c6].dll` | Our `avkon.dll` is built from the 2010 source: 4604 exports; Belle's has more. `tools/proteus_ordcheck.py`: this is the only missing ordinal of the 22 DLLs it imports (cone 86, avkon 84, eikcore 65 ordinals all present). A real EKA2 loader refuses it too (KErrNotSupported -5) | none yet: a Belle-only avkon export (signature unknown without the Belle `.def`) -- a core avkon change, not Proteus |
| PyS60 script shell 2.0 (`PythonScriptShell`) | plain Avkon + Python 2.5 | Starts and idles on VirtualBox, **no window group of its own after 240 s** (home screen stays on top, `img/compat-pys60-vbox.png`); P3 PASSed on QEMU on the earlier build (5de3790) | not diagnosed. The log shows `kf_Py_iad_client` (the IAD update client) loaded, a handled `KErrNotSupported` leave, then the main thread waits in its scheduler. Differences from the earlier run: VirtualBox, a disk made from the Qt disk (Qt installed beside it), the closure now built from the shell + all its DLLs | the closure for a SIS app must include the DLLs the SIS installs, not just the EXE: with only the EXE the run halted at `iaupdateapi ordinal 21 not emulated` (the DLL was in the ARM build but not staged) |

## What changed in Proteus for this

- `KMaxModules` 256 -> 512 (`os/src/proteus/proteusrt.h`). Soundwave's closure plus runtime-loaded plugins exceeded 256 loaded modules; the loader
  answered KErrNoMemory (-4). The other fixed tables the Sparkle notes named (guest chunks 64, threads 32, logons 8, `Duplicate` of sync handles)
  were not hit by any of these apps and were left alone.
- `User::Exit(reason != 0)` from the guest logs `[proteus] User::Exit(<reason>) from the guest:` and a backtrace (`proteusexec.cpp`).
- No stubs, no copy-protection handling.

## Notes for the next run

- A VirtualBox run needs a key press every 15 s (the PC image's lights timer, see `sparkle-gl-run.md`); `keep.sh` in the run scripts does it.
- The closure for an installed app = the EXE **and** every DLL the SIS installs (`proteus_closure.py <exe> <dll>...` into one `--out`).
- The disk for a run is the Sparkle disk (Qt installed) with `\sis` replaced; Qt apps share the three Qt SISes.

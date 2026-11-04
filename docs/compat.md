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
| LetsGolf2 (Gameloft, native C++ + GLES) | Avkon (non-Qt) game; **installed from its real SIS** (`n8fanclub.com_letsgolf2_1.0_signed_fp1.sis`, 167 MB, copied off the PinePhone's E:\BackupSIS through PhoneLab fs_read in 23 min; its EXE is byte-identical to `extracted/games/letsgolf2`) | Install **PASS**: the real installer (`sisinst`) answers INSTALLED (0) in about 8 min under VirtualBox; the EXE, the registration, `\private\2003B2CB\PAK` (323 MB) and the other data files all land on C: (every file in this SIS is `!:`). Run still **FAIL**: the guest process ends with `User::Exit(-25)` after 16.7 M instructions, no window group, nothing drawn | (1) **fixed**: C: was 256 MB, too small for the SIS plus its data; the run disk is now 1 GB (MBR + FAT32 at 1 MB). (2) **fixed, not the cause here**: Proteus had no VFPv2 short vectors (FPSCR.LEN/STRIDE); Let's Golf 2 builds its matrices with LEN=4 (`alf-plan.md`). (3) **open**: the exit. The new diagnostics show what it is not: the game's file-server traffic is only CreatePrivatePath x2, SessionToPrivate and SessionPath on a second RFs session (no data file is opened, so not a missing or short file), no WSERV or other server session is ever opened, and `User::Exit` runs with a nearly empty stack (no code addresses on it): the main thread returns -25 from the process entry, after `StaticCallsDone`, following a long libpthread (semaphore/mutex) phase. Next: find who returns -25 (a pthread wait/timeout result or a static initialiser) | (1) larger disk. (2) `CArmCpu::VfpOp` runs a vector data-processing op (VMLA/VMLS/VNMLA/VNMLS/VMUL/VNMUL/VADD/VSUB/VDIV, VMOV/VABS/VNEG/VSQRT register) as LEN scalar ops over bank-wrapping registers (8 singles / 4 doubles per bank, STRIDE 1 or 2, Fm in bank 0 = a scalar, destination in bank 0 = a scalar op); compares, converts, VMOV immediate, VLDM/VSTM and the transfers never vectorise. `tests/isa/vfpvec.c` (self-checking, in `tests/isa/run.sh`; qemu's A15 has no short vectors to diff against) PASS: multiply, scalar Fm, wrap inside the bank, stride 2, double, unary, MLA. (3) new log lines: `proteus FS:` (the first 300 file-server messages with function and 16-bit name arguments) and the raw stack at `User::Exit` |
| Calculator `calcsoft.exe` from the real 701 ROM (copied as `pcalc.exe`) | plain Avkon, Thumb, real Belle binary | FAIL at load: `ordinal 4611 not exported by avkon[100056c6].dll` | Our `avkon.dll` is built from the 2010 source: 4604 exports; Belle's has more. `tools/proteus_ordcheck.py`: this is the only missing ordinal of the 22 DLLs it imports (cone 86, avkon 84, eikcore 65 ordinals all present). A real EKA2 loader refuses it too (KErrNotSupported -5) | none yet: a Belle-only avkon export (signature unknown without the Belle `.def`) -- a core avkon change, not Proteus |
| PyS60 script shell 2.0 (`PythonScriptShell`) | plain Avkon + Python 2.5 | Starts and idles on VirtualBox, **no window group of its own after 240 s** (home screen stays on top, `img/compat-pys60-vbox.png`); P3 PASSed on QEMU on the earlier build (5de3790) | not diagnosed. The log shows `kf_Py_iad_client` (the IAD update client) loaded, a handled `KErrNotSupported` leave, then the main thread waits in its scheduler. Differences from the earlier run: VirtualBox, a disk made from the Qt disk (Qt installed beside it), the closure now built from the shell + all its DLLs | the closure for a SIS app must include the DLLs the SIS installs, not just the EXE: with only the EXE the run halted at `iaupdateapi ordinal 21 not emulated` (the DLL was in the ARM build but not staged) |

## The SIS flow (2026-11-04)

The P3 flow installs `c:\sis\*.sis` with the real installer; nothing else is staged for an app run except the ARM SYSTEM closure in
`c:\sys\bin\arm\` (made from the EXE the SIS itself installs: `proteus_closure.py <EXE>`; the EXE was taken out of the SIS with
`tools/sis_extract.py`) and `c:\sisapp.txt` (the EXE's name). The SIS defines everything else on disk. The earlier Let's Golf 2 run copied a loose
EXE to `\sys\bin` and had no data at all; that is no longer how an app is tested. A large package needs a large C: (SIS + temp + the installed
copy is about 3x its size): the run disk is 1 GB. After the install the SIS can be removed from `c:\sis` (the installed state kept) to re-run the
app on a new ISO without a second install: `c:\sisapp.txt` alone starts only the app.

## What changed in Proteus for this

- `KMaxModules` 256 -> 512 (`os/src/proteus/proteusrt.h`). Soundwave's closure plus runtime-loaded plugins exceeded 256 loaded modules; the loader
  answered KErrNoMemory (-4). The other fixed tables the Sparkle notes named (guest chunks 64, threads 32, logons 8, `Duplicate` of sync handles)
  were not hit by any of these apps and were left alone.
- `User::Exit(reason != 0)` from the guest logs `[proteus] User::Exit(<reason>) from the guest:` and a backtrace (`proteusexec.cpp`).
- VFPv2 short vectors (the Let's Golf 2 row); `User::Exit` logs the raw stack; the bridge logs file-server messages (`proteus FS:`).
- No stubs, no copy-protection handling.

## Notes for the next run

- A VirtualBox run needs a key press every 15 s (the PC image's lights timer, see `sparkle-gl-run.md`); `keep.sh` in the run scripts does it.
- The closure for an installed app = the EXE **and** every DLL the SIS installs (`proteus_closure.py <exe> <dll>...` into one `--out`).
- The disk for a run is the Sparkle disk (Qt installed) with `\sis` replaced; Qt apps share the three Qt SISes.

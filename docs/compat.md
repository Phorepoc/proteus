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
| LetsGolf2 (Gameloft, native C++ + GLES) | Avkon (non-Qt) game; **installed from its real SIS** (`n8fanclub.com_letsgolf2_1.0_signed_fp1.sis`, 167 MB; its EXE is byte-identical to `extracted/games/letsgolf2`) | Install **PASS** (real `sisinst`, INSTALLED (0), ~8 min under VirtualBox; EXE, registration, `\private\2003B2CB\PAK` 323 MB and the other data land on C:). Run: the `-25` start-up exit is **fixed**; the game now starts its Avkon UI, reads its option file, loads its resources and its CDL layouts, builds the video player (`CVideoPlayerUtility::NewL`, the intro video) and stops there with **BITGDI 10** (`CFbsBitGc::SetClippingRegion`, called from `CDirectScreenAccess::StartL`): the DSA region is the landscape screen (640x360, rotated mode 2) but the guest's `CFbsScreenDevice` is not that size. No window group yet (P3 FAIL: the app exits on the panic) | (1) **fixed, the -25**: see "Root cause of the -25" below. (2) **fixed**: `libstdcpp.dll`, `hal.dll`, the second CDL layout `2001e2b1.dll` (uiaccel, `layoutinst_nhd4_uiaccel`), `akninit`, `eikcdlg`, `avkonfep` + `aknfepuiavkonplugin`, the ICL codecs + `bitmaptransformsrefplugin` were not in the ARM closure (the closure follows imports, not `RLibrary::Load`/ECom loads): each stopped the run at a `no ARM file` / `ordinal N not emulated` until staged. (3) **open**: the DSA panic above (the video player needs a DSA screen device of the rotated size in the guest, and no MMF video controller exists for ARM yet, so the intro video cannot play either) | (1) `EExecProcessGetDataParameter` returns the length. (2) staged (the ROM carries the ARM `hal.dll`; the rest are disk files). (3) none yet |
| Calculator `calcsoft.exe` from the real 701 firmware, set up as the ROM app it is: the EXE in `c:\sys\bin\`, its registration `c:\private\10003a3f\import\apps\CalcSoft_reg.rsc` and `c:\resource\apps\Calcsoft.r01/.mif/.mbm` (from the other device's dump; the 701's own `.rsc` set is not in the repo) | plain Avkon, Thumb, real Belle binary | **P3 PASS, UI shown and working** (`img/compat-calculator-vbox.png`; `7 * 6 =` tapped through VirtualBox mouse integration gives 42, `img/compat-calculator-vbox-42.png`): window group `10005902 Calculator`, still running | (1) `avkon[100056c6].dll` ordinal 4611 (Belle's `CAknTasHook::AddTimestampL`): implemented in core 840e0ca; the ARM avkon.dll with it was staged. (2) the `-25` (same root cause as Let's Golf 2) | (1) core 840e0ca. (2) `EExecProcessGetDataParameter` returns the length |
| PyS60 script shell 2.0 (`PythonScriptShell`) | plain Avkon + Python 2.5 | Starts and idles on VirtualBox, **no window group of its own after 240 s** (home screen stays on top, `img/compat-pys60-vbox.png`); P3 PASSed on QEMU on the earlier build (5de3790) | not diagnosed. The log shows `kf_Py_iad_client` (the IAD update client) loaded, a handled `KErrNotSupported` leave, then the main thread waits in its scheduler. Differences from the earlier run: VirtualBox, a disk made from the Qt disk (Qt installed beside it), the closure now built from the shell + all its DLLs | the closure for a SIS app must include the DLLs the SIS installs, not just the EXE: with only the EXE the run halted at `iaupdateapi ordinal 21 not emulated` (the DLL was in the ARM build but not staged) |

## The SIS flow (2026-11-04)

The P3 flow installs `c:\sis\*.sis` with the real installer; nothing else is staged for an app run except the ARM SYSTEM closure in
`c:\sys\bin\arm\` (made from the EXE the SIS itself installs: `proteus_closure.py <EXE>`; the EXE was taken out of the SIS with
`tools/sis_extract.py`) and `c:\sisapp.txt` (the EXE's name). The SIS defines everything else on disk. The earlier Let's Golf 2 run copied a loose
EXE to `\sys\bin` and had no data at all; that is no longer how an app is tested. A large package needs a large C: (SIS + temp + the installed
copy is about 3x its size): the run disk is 1 GB. After the install the SIS can be removed from `c:\sis` (the installed state kept) to re-run the
app on a new ISO without a second install: `c:\sisapp.txt` alone starts only the app.

## Root cause of the -25 (Let's Golf 2 and the Calculator)

Both apps ended the same way because the guest never received its command line. apparc passes it in process parameter slot 1 (`RProcess::SetParameter`);
the guest reads it with `User::ParameterLength(1)` + `User::GetDesParameter(1, aDes)`, and euser's `GetDesParameter` is

    TInt ret = Exec::ProcessGetDataParameter(aSlot, (TUint8*)aDes.Ptr(), aDes.MaxLength());   // us_exec.cpp:6891
    if (ret < 0) return ret;
    aDes.SetLength(ret);                                                                       // the LENGTH comes from the exec

so the executive must return the number of bytes copied (sexec.cpp `ExecHandler::ProcessGetDataParameter` returns `len`). Proteus's bridge (`BridgeKernel`,
`proteus.cpp`) forwarded the call to the native `User::GetDesParameter`, which returns `KErrNone` (0) -- so the descriptor length became 0, the data was
there but "empty", and `CApaCommandLine::GetCommandLineFromProcessEnvironment` read an empty buffer: end of stream, `KErrEof` (-25), which the app's start-up
(`CEikonEnv::ConstructAppFromCommandLineL`) turned into `User::Exit(-25)` before any window or file access. Both PinePhone-native runs get the real exec
and are fine. Evidence (trace added): `qexec EExecProcessDataParameterLength r0 1 ...`, `qexec EExecProcessGetDataParameter r0 1 r1 ... r2 38`
(slot 1, 0x38 bytes max) then `EExecProcessGetDataParameter r0 3` (GetTIntParameter, the app's id) and the app goes on to create its CCoeEnv.
The bridge now returns `d.Length()` (or the negative error).

Other things the now-running apps needed (each found by its own log line, each fixed at the cause):

- `EExecHalFunction` was answered locally for kernel group 0 only and `KErrNotSupported` otherwise; eikcore's display queries and `hal.dll`'s attributes failed with -5,
  which Let's Golf 2 showed as "Feature is not supported". The bridge now makes the same executive call on the native kernel (`UserSvr::HalFunction`; guest pointers are
  this process's memory). The ARM `hal.dll` (the real `hal_main`/`userhal` sources) is in the ROM next to the ARM euser/efsrv (`recipes/stage_arm.sh`, `registry/components_proteus.py`).
- RMsgQueue handles a server hands over in a reply (`RDirectScreenAccess`: the window server's two DSA queues) are adopted like chunk handles
  (`EExecMsgQueue*` on an unknown native-looking handle), instead of halting with "exec 186 is not emulated".
- Not Proteus, closure: dynamically loaded DLLs (ECom/CDL plug-ins, `akninit`, `eikcdlg`, `avkonfep`, ICL codecs) are not found by following imports; stage them with `proteus_closure.py --extra`.
- New bounded traces: `qexec` lines for every message-queue and process-parameter exec, and `exec <name> -> <result>` for any exec that answers -4, -5 or -25
  (which call, which caller).

## What changed in Proteus for this

- `KMaxModules` 256 -> 512 (`os/src/proteus/proteusrt.h`). Soundwave's closure plus runtime-loaded plugins exceeded 256 loaded modules; the loader
  answered KErrNoMemory (-4). The other fixed tables the Sparkle notes named (guest chunks 64, threads 32, logons 8, `Duplicate` of sync handles)
  were not hit by any of these apps and were left alone.
- `User::Exit(reason != 0)` from the guest logs `[proteus] User::Exit(<reason>) from the guest:` and a backtrace (`proteusexec.cpp`).
- VFPv2 short vectors (the Let's Golf 2 row); `User::Exit` logs the raw stack; the bridge logs file-server messages (`proteus FS:`).
- The `-25` fix, `HalFunction` to the native kernel, message-queue adoption, the ARM `hal.dll` in the ROM (see the root-cause section). No stubs, no copy-protection handling.

## Notes for the next run

- A VirtualBox run needs a key press every 15 s (the PC image's lights timer, see `sparkle-gl-run.md`); `keep.sh` in the run scripts does it.
- The closure for an installed app = the EXE **and** every DLL the SIS installs (`proteus_closure.py <exe> <dll>...` into one `--out`).
- The disk for a run is the Sparkle disk (Qt installed) with `\sis` replaced; Qt apps share the three Qt SISes.

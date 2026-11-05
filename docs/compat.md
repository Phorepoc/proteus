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
| Soundwave (Qt Quick / QML) | Qt 4.7 + QtMobility app | P3 **PASS** (running, window group `200409a3`). Starts Qt, loads its QML, then stops at a white page: `module "QtMultimediaKit" is not installed` (`img/compat-soundwave-vbox.png`) | (1) **fixed**: the ARM module table held 256 modules; Soundwave's closure (Qt + CDL/ECom plugins) needs more, the 257th `RLibrary::Load` (`101fe2aa.dll`, a CDL plugin) answered -4 and `qt_init` failed ("Eikon application construct failed (-4)"). (2) **open, not Proteus**: the app imports the QML module `QtMultimediaKit`; the Qt Mobility package staged on the disk (`00b-qtmobility.sis`) installs it as `\resource\Qt\imports\QtMobility\multimedia`; copying it under `QtMultimediaKit\` (qmldir + `declarative_multimedia.qtplugin`) did not make Qt find it. The Qt Mobility build the app was made for (1.2, `import QtMultimediaKit 1.1`) is not the one on the disk | (1) `KMaxModules` 256 -> 512 (`proteusrt.h`; the import slot encoding allows 1023). (2) not the package: both Qt Mobility 1.2.1 SIS backups (`qtmobility-1.2.1.sis`, `pub-qtmobility-1.2.1.sis`) install the plugin only as `\resource\qt\imports\QtMobility\multimedia` too, and `declarative_multimedia.dll` carries no URI check (it registers Audio/Video/SoundEffect under the URI it is loaded for), so a `QtMultimediaKit\qmldir` copy should load it: the open question is the QML import-path lookup (next: `QML_IMPORT_TRACE=1`) |
| LetsGolf2 (Gameloft, native C++ + GLES) | Avkon (non-Qt) game; **installed from its real SIS** (167 MB; EXE byte-identical to the 701's) | Install **PASS** (real `sisinst`). Run **P3 PASS** (window group `2003b2cb Lets Golf 2` after 240 s). With the codec pack installed (`codecpack-x32.sis`, release v0.1.0, installed by the same SIS flow) both intro clips PLAY through the hosted native MMF controller (`phevidctl`): Logo 7.3 s and intro 60.8 s, MPEG-4 Part 2 SP 640x360 + AAC-LC 48 kHz, 1441/1459 pictures drawn, audio 60.6 s. Then Input Name, title, main menu, Single Player, Select Player, hole preview, **a playable hole** (Mount Olympus, hole 5: HUD, wind, minimap, aim, power meter on a swing gesture) in the rotated landscape mode, ~3-11 fps interpreted. Shots: `img/compat-letsgolf2-*.png`. Without the codec pack the clips fail to open (`init complete -1`: no decoder) and the game skips them on its own error path. | (1) -25: see below. (2) staged: libstdcpp, hal, CDL layouts, akninit, eikcdlg, avkonfep, ICL codecs. (3) **DSA BITGDI 10, fixed**: the ARM `scdv.dll` was the generic screen driver, whose draw device refuses any orientation but the panel's (`CDrawBitmap::SetOrientation`), so the DSA gc stayed 360x640 while WSERV's default mode is 640x360 rotated 90 (evidence: `proteus WSR` lines: `op 0x11 -> 640 360 1`, HAL 360x640). The ARM scdv is now built like the PC's (OMAP draw devices + `phescdv` factory; `armb.sh scdv`). (4) **GL**: `proteus_closure.py` no longer stages libEGL/libGLES* (they run natively; the ARM copy ran Mesa without phgl and the game faulted). (5) **The game's own in-process server** (`SoundStreamServer`, CServer2 in a game thread, the other threads its clients): guest-side servers/sessions/messages in the executive (`GuestIpcExec`, kernel/sipc.cpp semantics) plus named global semaphores/mutexes/condvars of the process (`SoundStreamServerSemaphore` was created and opened by name). (6) `f32agent.dll`/`f32agentui.dll` ARM staged (the MMF CAF plug-ins). | (3) scdv from adaptation-pc's registry; (4) closure tool; (5)(6) Proteus executive |
| Calculator `calcsoft.exe` from the real 701 firmware, set up as the ROM app it is: the EXE in `c:\sys\bin\`, its registration `c:\private\10003a3f\import\apps\CalcSoft_reg.rsc` and `c:\resource\apps\Calcsoft.r01/.mif/.mbm` (from the other device's dump; the 701's own `.rsc` set is not in the repo) | plain Avkon, Thumb, real Belle binary | **P3 PASS, UI shown and working** (`img/compat-calculator-vbox.png`; `7 * 6 =` tapped through VirtualBox mouse integration gives 42, `img/compat-calculator-vbox-42.png`): window group `10005902 Calculator`, still running | (1) `avkon[100056c6].dll` ordinal 4611 (Belle's `CAknTasHook::AddTimestampL`): implemented in core 840e0ca; the ARM avkon.dll with it was staged. (2) the `-25` (same root cause as Let's Golf 2) | (1) core 840e0ca. (2) `EExecProcessGetDataParameter` returns the length |
| PyS60 script shell 2.0 (`PythonScriptShell`) | plain Avkon + Python 2.5 | **P3 PASS on VirtualBox** (window group `20022eec PythonScriptShell`, the skinned Python screen with the interpreter banner, `img/compat-pys60-vbox.png`) | Not a Proteus bug: the ARM build directory used for the closure was stale. (1) its `iaupdateapi.dll` (9272 B, Oct 23) was the 2010 IAD **stub**: `CIAUpdate::NewL` leaves KErrNotSupported (the handled -5), PyS60's IAD helper then waits in a scheduler wait nothing ends (`proteus wait 1`, no window group); core's registry builds the real client (17308 B) since dd4aa30. (2) with the real client the next stop was `drtrvct2_2 ordinal 28 not emulated`: `Python25.dll` (RVCT-built) imports the RVCT runtime, core ships `drtrvct2_2` but the stale build lacked it (the closure printed it as unresolved). Fix: build `iaupdateapi iaupdatetools drtrvct2_2` for ARM, re-stage; `proteus_closure.py --strict` now fails on unresolved DLLs | the closure for a SIS app must include the DLLs the SIS installs, not just the EXE (with only the EXE: `iaupdateapi ordinal 21 not emulated`); rebuild the ARM components from current core before staging |

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


## Let's Golf 2 to a playable hole (2026-11-04)

Evidence trail (one VM, `golf-compat`, VirtualBox):
- Sizes: `proteus HAL` (display group 4) = the panel 360x640; `proteus WSR` (screen-device replies of the window server) = default screen size/rotation 640x360, rotation 1. The guest's DSA clips to 640x360 and `CFbsBitGc::SetOrientation` returned EFalse in the generic scdv, so `iDefaultRegion` stayed 360x640: `IsContainedBy` failed = BITGDI 10. The generic driver is right for the NGA/GCE case only; this board's WSERV is non-NGA and rotates in software, so the ARM driver must offer the rotations too, as the native one does.
- Video: the PC image already carries the MMF video controller `phevidctl` (ECOM 0xE70157D1; MP4 demux, DevVideo, DSA bitmap path) and the hosted controller design carried it without change: the ARM `CVideoPlayerUtility` (window, DSA) talks to the same native WS session, so the window/region handles the controller receives are native ones. What the ROM lacks is the decoder: `phevdecsw` + `pheffmpeg` ship in the codec pack (LGPL / patents: never in an image). Format of the clips (`\private3B2CBideo`): ISO MP4, MPEG-4 Part 2 Simple Profile L1 640x360 ~24 fps + AAC-LC 48 kHz stereo; the 701 decodes them in hardware. With `codecpack-x32.sis` in `\sis` (installed by `sisinst` before the app) both play; without it `FindDecodersL` fails and the game skips to Input Name.
- Run recipe: closure from the EXE (no GL DLLs) + `scdv`, `f32agent`, `f32agentui` in `\sysinrm`; `\sis\codecpack-x32.sis`; keep-alive key every 15 s; coordinates for VirtualBox absolute mouse are framebuffer pixels (360x640, the landscape UI is rotated 90 degrees clockwise on it).

Open: swing gesture timing at 3 fps (the meter runs; no ball flight captured yet), steady-state speed (2.5-11 fps), screenshots are taken from the portrait framebuffer. Guest servers: `MessageCompleteWithHandle`, `MessageKill`, `TransferSession`, `MessageOpenObject` halt loudly (not emulated); a server thread's death does not terminate its clients' requests.

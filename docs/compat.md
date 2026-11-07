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

Open: swing gesture timing at 3 fps (the meter runs; no ball flight captured yet), steady-state speed (2.5-11 fps), screenshots are taken from the portrait framebuffer. Guest servers (`GuestIpcExec` in `proteusexec.cpp`, checked by `tests/proteus/guest_servers.cpp`, 82 checks): `MessageComplete`, `MessageCompleteWithHandle`, `MessageKill`, `MessageOpenObject`, `TransferSession` and `SetSessionPtr` follow `kernel/sipc.cpp`; a named server dies with its thread (`DServer::Close`: pending Receive -> KErrCancel, every session detached, queued and accepted requests -> KErrServerTerminated, none for a session the client closed or a dead client), an unnamed one with the process. Open: the handle space is one table per process, so a handle "opened" in the server is the client's value with one more reference (mutexes, semaphores, condition variables, message queues, threads, chunks; chunks and threads are not reference counted here); a native-kernel handle halts loudly ("not emulated"); `ELocal` handle protection does not exist; sessions have no message-slot limit (`KErrOverflow`) and no security policy; the P0-P3 VM regression of this change is the next section.


## Regression after the guest-server rework (2026-11-05, aaefc21, VirtualBox)

PC image built with the Proteus of aaefc21 (`SSM_TEST=proteus`), sources of the PC layers as of the morning of 2026-11-04 (the `protgl-src` volume's core is that old; the newest adaptation-pc phgl needs a Mesa svga build the volume does not have), VirtualBox 2 CPUs, 1 GB, VMSVGA, AC97 with the WAV capture, key every 15 s. P0, P1, P2, P2b, P2c, P2d, P2f PASS on every disk.

| App | Result on aaefc21 | Notes |
|---|---|---|
| Sparkle | **P3 PASS** (`Sparkle.exe` running after 240 s, window group `20030f70`); **music plays**: the controller is hosted natively (`thread 3 is hosted natively`, `native LoadController -> 0`), the AC97 capture's one-second peaks stay 3203..29083 through 60 s of play in a level, no `not emulated`, no guest panic, no `KErrServerTerminated` seen. Shot: `img/compat-sparkle-reg-20261105.png` | the old run disk lacked `f32agent.dll`/`f32agentui.dll` in `\sysinrm` (`RLibrary::Load f32agent.dll -> -1`, the Switching-the-music line and then silence): a stale disk, not the change; staged from the ARM build, music back |
| Let's Golf 2 (codec pack) | **P3 PASS** (window group `2003b2cb`), the intro clip plays to its end (`vc done: fed 1459 delivered 1459 drawn 1438`, audio 60650 ms, `err 0`) and the game reaches Input Name (`img/compat-letsgolf2-reg-20261105.png`); its sound server (sessions in game threads) ran with no panic, no `KErrServerTerminated`, no unemulated exec. Not driven past Input Name in this run (menu and hole not re-checked) | stale disk again: the Nov 4 closure lacked `2001e2b1.dll`, `akninit` and the ICL set; staged with `proteus_closure.py --extra` for the ARM build dir; the ARM `libEGL`/`libGLES*` the extras pulled in must be removed (they run natively) |
| Calculator | not re-run: the staged ARM `avkon.dll` of the run disk (and of the `protgl-arm` volume, Oct 23) lacks ordinal 4611 (`load failed: ordinal 4611 not exported by avkon`), the fix of the Calculator row; an ARM rebuild of avkon from current core failed in `cpp` inside the container (not chased) | stale ARM build, not Proteus |
| PyS60 | **P3 PASS** (window group `20022eec PythonScriptShell`; `img/compat-pys60-reg-20261105.png`) | the old run disk had the 9272 B `iaupdateapi.dll` stub and no `drtrvct2_2.dll` (the documented stale-ARM cause: `proteus wait 1`, no window group); with the current ARM files staged it passes |

Conclusion: no regression found for Sparkle, Let's Golf 2 and PyS60 from aaefc21; every failure on the way was a stale staged ARM file on an old run disk. Left: Let's Golf 2 menu to a hole, the Calculator re-run (needs a current ARM avkon), a clean build of the current phgl (Mesa svga archive) in `protgl-pc2`.

## Guest thread priorities (2026-11-05)

Guest threads are scheduled with the EKA2 priority model (proteusexec.cpp, "priorities" section; test `tests/proteus/priorities.cpp`). Before this the calls were only logged.

**Mapping.** A thread's absolute priority (0..63) = `ThreadPriorityTable[(process priority << 3) + (thread priority + 8)]` for the relative thread priorities (`DThread::CalcDefaultThreadPriority`,
kernel/eka/kernel/sthread.cpp:442, table at :429); an absolute thread priority (`EPriorityAbsolute*`, kernel value >= 0) is used as is (capped at 63). Without the ProtServ capability (bit 10 of the
EXE header's first capability word) the result is capped at 24 (`KPrioritySystemServerMore`, sthread.cpp:422). `RThread::SetPriority` -> `DThread::SetPriority` (sthread.cpp:1009) converts
`TThreadPriority` to the kernel's `TThrdPriority` (kern_priv.h:484); an invalid value panics the caller KERN-EXEC 14 (`EBadPriority`). `RProcess::SetPriority` / `RThread::SetProcessPriority`
-> `ExecHandler::ProcessSetPriority` (sexec.cpp:436): only Low / Background / Foreground / High are valid (else KERN-EXEC 14), and every thread of the process is recomputed
(`DProcess::SetPriority`, sprocess.cpp:268). `RMessagePtr2::SetProcessPriority` accepts Background / Foreground only (sipc.cpp:2026; the PriorityControl process flag is not modelled), else
`KErrPermissionDenied`. The getters answer from the same state (`ConvertThreadPriority`, sexec.cpp:367). The process priority starts as the EXE header's `iProcessPriority`
(0x78, `DProcess::Create`: `ConvertPriority`; Foreground when the header has none); the main thread and every created thread start at `EThrdPriorityNormal` (sexec.cpp:1938:
the priority field of the creation info is overwritten by the kernel, so Proteus ignores it too). Not modelled: the ProtServ check on `RThread::SetPriority(EPriorityAbsoluteRealTime*)`,
the cross-process rules (one guest process), priority inheritance through mutexes and the cleanup-queue boost (`TThreadCleanup`).

**Scheduler.** `PickNext`: the ready thread with the highest absolute priority; among equals the first after the running thread in index order (the running one counts last), i.e. round robin by
time slice as before. Used by `Reschedule` (the running thread blocked or died) and `Preempt` (end of a time slice, `KSliceInstructions` = 1 << 17: the running thread stays a candidate, so a
woken higher-priority thread takes over here and a lower one does not run). New: after every executive call, `ExecCall` ends the slice at once (`iNextSlice = iSteps`, the mechanism the long
GL call already used) when a thread of higher priority than the running one is ready (a signal, a resume, a mutex hand-off, a priority change), so the preemption happens at the next
instruction, not at the slice end. A request completed by a native server is credited in `Preempt` / `Reschedule` (`AttributeNative`) as before, so it too preempts at the next slice end (or at
once after a long native GL call, which ends the slice).

**Hosted native threads** (the MMF controller helper) are not in the guest scheduler: they run concurrently on the native side at the native thread's normal priority, whatever the guest asks.
`SetPriority` on them changes only the guest-visible value. So a guest thread can no longer starve them, and they cannot be starved by the guest.

**Starvation.** None added or removed: like EKA2, a ready thread of higher priority that never blocks starves every lower one (the slice only rotates equals). In the kernel the idle thread and
the timer/DFC machinery sit around it; in Proteus the native requests and timers are polled at every slice end and by `Reschedule`, so a busy guest thread cannot stop timed waits or native
completions from being credited, only lower-priority guest threads from running. If an app depends on a lower-priority thread running while a higher one spins (it would hang on a device too,
but Proteus slices are coarser than a device's 1 ms tick), look at `[proteus] ... abs priorities` lines in the log first. Nothing known depends on this yet.

**Re-run after this change** (the app regression, on the PC image in VirtualBox): Sparkle P0-P3 including music and the effects stream rate, Let's Golf 2 to a hole, the Calculator, the
SIS installer flow, About. Watch for hangs after the first UI frame (a thread that used to share the processor now waits on a higher one) and for the wake-up latency numbers (`ThreadStats`).

## Regression after the guest priorities (2026-11-05, a5c3fb9, VirtualBox)

PC image of Proteus a5c3fb9 (`SSM_TEST=proteus`, PC layers as of the morning of 2026-11-04 as before), VirtualBox 2 CPUs, 1 GB, VMSVGA, AC97 with the WAV capture, key every 15 s. P0-P2f PASS (the QEMU board run too).

**Run-disk staging, fixed in the tools (no hand staging).** `tools/proteus_closure.py --app sparkle|letsgolf2|pys60|calculator` adds the run-time loads no import names (`APP_EXTRAS`: Sparkle `f32agent`/`f32agentui`;
Let's Golf 2 `akninit`, `2001e2b1`, `eikcdlg`, `avkonfep`, `aknfepuiavkonplugin`, the ICL codecs + resolver, `bitmaptransformsrefplugin`, `f32agent*`; PyS60 `iaupdateapi`, `drtrvct2_2`; all: `scdv`), the ARM `libEGL`/`libGLES*` stay out (`NATIVE_GL`),
`euser`/`efsrv`/`hal` are the ROM's (`ROM_ARM`), and `--strict` fails the staging on any other unresolved DLL. `tools/proteus_stage.sh NAME APP sis|installed|exe DIR` builds the run disk from the docker volumes
(base disk + packages + the closure of the EXE, taken out of the SIS with `sis_extract.py`; `BASE=` a blank/installed base, `DATA=` a tree for ROM-app registrations). Sparkle's base disk is the installed state, so it takes `installed` (a SIS
on it fails with -30478: already installed).

| App | Result on a5c3fb9 | `EExecThreadSetPriority` lines (request -> absolute priority now, thread/current) |
|---|---|---|
| Sparkle | **P3 PASS** (`Sparkle.exe` running after 240 s, window group `20030f70`); driven Start -> Play -> Play Quest -> OK -> menu; **music plays**: AC97 capture one-second peaks 3082..14749 over 350 s, no `not emulated`, no guest panic, no hang after the first UI frame. `img/compat-sparkle-pri-20261105.png` | `thread 2 (cur 0) -> 20; abs 14/12`; `thread 3 (cur 2) -> 0; abs 12/14` (nothing else) |
| Let's Golf 2 (codec pack) | **P3 PASS** (window group `2003b2cb`) on the installed disk; both clips play (`vc done`: 220 pictures/6.8 s and 1459 fed, 1440 drawn/69.9 s), Input Name reached (`img/compat-letsgolf2-pri-20261105.png`), no panic, no `not emulated`, no `KErrServerTerminated`. **Not driven to a hole**: on Input Name the screen shows only the left 360 px of the 640x360 UI, keys (Enter, F6) and taps at the visible keys did not change it, a tap at the rotated OK position blanked the screen for ~30 s and returned to Input Name. Open | `thread 1 (cur 0) -> 0; abs 12/12` (twice, once per CPU) |
| PyS60 | **P3 PASS** (window group `20022eec PythonScriptShell`, `img/compat-pys60-pri-20261105.png`); the install of both SISes through the real installer passes | none |
| SIS installer flow | PyS60's two packages and the codec pack install (`create 0 exit type 0 reason 0`). Let's Golf 2's own 167 MB package did **not** finish inside P3's 600 s install limit on this run (`exit type 3`, then the app step FAIL); not compared with aaefc21 (no time), so whether the priorities slowed it is **unknown**: to A/B on `reg.iso` (aaefc21) | n/a |
| About | not run | |
| Calculator | **FAIL, not priority related**: the ARM `avkon.dll` now rebuilt from current core exports 4631 (ordinal 4611 present, `proteus_ordcheck` shows nothing missing), but the app panics `USER 23` (`lr=euser.dll+0x1e990 User::StringLength`, in `bafl` TDes8::Copy under `eikcoctl`/`avkon`) right after its `EikAppUiServer` message (`fn 10, 8 args`). The same panic with the aaefc21 ISO (`reg.iso`), so it is neither the priorities nor the guest-server rework; a run on a pre-rework ISO is the open A/B. The registration + resources are staged from the extracted dump (`DATA=calc-data`) | none (died first) |

**ARM avkon rebuild (why it failed before).** The `protgl-src` volume's core was the Nov 4 snapshot, which has no `avkon_later` sources at all: the avkon the container built had no ordinal 4611. With current core's `os/` + `registry/`
unpacked over it, `phe component build -arch arm avkon` (the `armb.sh` mounts: `protgl-src` at `/w`, core's `symbian` as a read-only subpath mount at `/symbian`, `protgl-arm` at `/w/build` and `/build`, `ARM_VFP=1`, `PATH` with `/opt/arm-gnu/bin`
and `/opt/symbian-hosttools`) builds in the container (rc 0, `avkon.dll` 1363856 B). No `cpp` failure reproduced; the earlier one was most likely the stale core / a missing `/symbian` mount, not the source.

**Priority findings.** No priority-related hang or deviation seen: the apps request very little (Sparkle one boost to 20 on a Qt thread and one back to Normal, Let's Golf 2 only `Normal`, PyS60 and the Calculator none), the main thread never waits on a thread that
has to run below it. Nothing changed in proteusexec.cpp.

**Environment note.** VirtualBox refused to start (`VBoxDrvStub ... SystemExtendedHandleInformation failed`, 507 MB handle table) while a stray `find / -name eshell.mmp` of 21:37 (pid 7008, 12.6 million handles, not from this run) was alive; it started on a later attempt without the process
being touched. That process should be killed by its owner.

Left: Let's Golf 2 from Input Name to a hole (the cropped Input Name screen; touch mapping), the Calculator panic A/B, the long SIS install timing A/B, About, the clean build of the current phgl.

## Follow-up (2026-11-06, VirtualBox, PC image `lim.iso` = Proteus with the install-limit change)

**1. Let's Golf 2 install limit: fixed, install confirmed.** The limit is `RunTimed(_L("sisinst.exe"), pkg, 600, ...)` in `os/src/proteus/proteus.cpp` (P3). `RunTimed` leaves a process that is still running at the limit
running and reports `EExitPending` (= "exit type 3"), so the 600 s mark only made P3 stop waiting; it did not kill the installer. Physical bound: the 167 MB package unpacks through the real installer at the speed of the
VirtualBox disk and the PC image's file server; it needs well over 600 s. The limit is now 14400 s (4 h ceiling). `RTimer::After` takes a 32-bit microsecond interval (35 min at most), so `RunTimed` now waits in chunks
of 1800 s (before, any value above 2147 s would have wrapped). Result: both packages install (`P3 install codecpack-x32.sis: create 0 exit type 0 reason 0`, `P3 install letsgolf2.sis: create 0 exit type 0 reason 0`), then
`LetsGolf2.exe` runs, window group `2003b2cb`, **P3 PASS at 849 s** after VM start (boot + both installs + the 240 s app wait; the install itself took under 10 minutes). Not compared with aaefc21, so whether the
priorities slowed the install is still unknown (the install ends inside one run, the A/B is a matter of one more run on `reg.iso`).

**2. Calculator `USER 23`: not Proteus; it is the ARM files staged on 2026-11-05.** USER 23 is `ETDes8Overflow` (e32panic.h), a `TDes8::Copy` into a too-small buffer. The EikAppUiServer message before it is
`EEikAppUiGetDebugPreferences` (fn 10; enum in `EIKSRVS.H`), the client's `REikAppUiSession::GetDebugPreferencesL`: `TIpcArgs(8, &bufPtr)` (8 = `KDefaultBufSize`), the server completes 0 (the preferences fit)
and the bridge hands the client the same TDes8 it wrote to (`proteus IPC: !EikAppUiServer session 11 fn 10 flags 20 args 8 <TPtr8> 70 0 sync`, completed 0). The argument marshalling is therefore correct
(arg 1 is a writable 8-bit descriptor, flag 0x20; the guest and host descriptors share their layout); the panic is a later copy (`lr` = `euser.dll+0x486d4`, the panic helper, from `bafl.dll+0xa00c`, under `eikcoctl`).
A/B on this very ISO (Proteus unchanged), same registration/resources: the **Nov 4 ARM set** (`compat-calc.img`: avkon 1358872 B, eikcoctl 658916 B) **runs the Calculator** (calcsoft.exe running after 240 s, UI
drawn: `img/compat-calculator-oldarm-20261106.png`; the P3 verdict says FAIL only because the stale disk's `sisapp.txt` had said `pcalc.exe` and I set it by hand for this A/B), the **Nov 5 set** (avkon 1363856 B,
eikcoctl 658832 B) panics `USER 23`. Swapping only eikcoctl back to the Nov 4 file: still panics. Swapping only avkon back to the Nov 4 file (the Nov 5 rest unchanged): **no panic, calcsoft.exe running after 240 s**. So the fault is in the ARM avkon.dll rebuilt from current core in the `protgl-arm` volume, not in the guest->host conversion. Per the rules this is a native question first (does the same avkon from the same
build run the Calculator on the native ARM side?): not Proteus' to patch.

**3. Let's Golf 2 Input Name, only the left 360 px: evidence, cause not isolated.** The WSERV screen is in mode 0 (360x640) while the game draws its 640x360 GL surface: `proteus WSR: op 3 -> 168 280` (360x640) at the
time the game's window group exists. The cap server's screen-mode decision for the game's window group: `sgc wg 42 full 1 orientSpec 4 landscape 8 spLayout 0x8cc01a4` (the game asked for landscape, as Sparkle does:
`orientSpec 4 landscape 8`) and then `sgc SetLayoutL top wg 42 (sp index 3) -> mode 0 (last set 0)`: `CalculateScreenMode(ETrue, specified, landscape, unset)` returned mode 0 for a landscape request. No Proteus
code touches orientation (the guest talks to the native WSERV / cap server over the bridge), the game's flags are the same as Sparkle's, so this is the PC image's screen-mode configuration or the state the cap server
sees (hardware layout state / alternate screen mode of the layout config), not something Proteus converts. The picture is the unrotated 640x360 surface blitted at the bottom of the 360x640 window (`img/compat-letsgolf2-inputname-20261106.png`).
Next: log `iAvkonEnv->SettingCache().HardwareLayoutState()` and the layout config's modes in `CEikSgcServer::SetLayoutL` (core `akncapserver_trace`) and compare with the Sparkle run. Keys and taps were not retried (they go to the
same window, whose size is the portrait one).

**4. About: not run** (no time left in the 60 minutes).

## Calculator USER 23: clean ARM rebuild A/B (2026-11-06)

The `protgl-src` volume's core was a mixed state (54c5824 + uncommitted edits; adaptation-pc/mesa/tls older than the lock). Its uncommitted edits are saved in `patches/` (`protgl-src-core-54c5824-uncommitted.patch`, `protgl-src-pdk-776ba8e-uncommitted.patch`).
Clean redo: core checked out at 1011bd4, pdk at 415ac54 (board.lock), `adaptation-pc` df40744 (its pin of core 1011bd4; board.lock's 057af3a still EXTENDs the removed `pheget`, so it cannot build against 1011bd4) + `catalogue` 0dc3532 + mesa-symbian 4910a10 + tls d3d54fa as git archives (stale
copies kept as `*.stale-nov5`); the ARM `mmp-build` and `defs` were moved aside (`/w/build/arm/mmp-build.stale-nov5`, `/w/build/stale-nov5`), so every object was recompiled; the build needs core's `symbian` mounted at `/symbian`
(without it `cpp` fails on avkon.mmp, which was the Nov 5 "cpp failure"). Rebuilt: `libstdcpp avkon eikcoctl eikcoctllaf` (avkon.dll 1363956 B, eikcoctl.dll 660356 B). The rest of the closure is unchanged from the volume.
Result on `lim.iso` (VirtualBox, `compat-calculator-cleanarm-20261106.png`): **PASS**: `proteus P3 calcsoft.exe: create 0 exit type 3 reason 0 (running after 240 s)`, UI drawn, no USER 23. Conclusion: the panic came from the mixed avkon build, not from core 1011bd4 or Proteus. About: not run.

## Host GPU, JIT vs interpreter (2026-11-07)

PC image ISO of 2026-11-07 17:05 (newest Proteus + kernel), VirtualBox 2 CPUs, 1 GB, `VBOX_3D=1` (VMSVGA + SVGA3D, host GPU), one VM at a time under the measurement lock. Let's Golf 2 from its real SIS
(`letsgolf2.sis` + `codecpack-x32.sis`, 1 GB disk staged with `tools/proteus_stage.sh lg3 letsgolf2 sis lg2` and `BASE=lg-base.img`, ARM closure from the current ARM build). The interpreter runs use a copy of the
installed disk (post-install state of run 1, `c:\sis` emptied, so no reinstall) plus `c:\proteus\interp.txt` (`interpreter`); the JIT control on that same installed disk is run 3.

| run | disk | P3 | first GLSTAT (after VM start) | steady fps (GLSTAT, frames 300-2500) | per frame (GL / present / guest code) |
|---|---|---|---|---|---|
| JIT, host GPU | fresh (2 installs + app) | PASS, window group `2003b2cb` | 871 s (includes both installs, ~605 s+) | 18.1-21.4 (19.4 typical) | 0.2-0.3 / 19-29 / 21-33 ms |
| JIT, host GPU | installed | PASS | 366 s | 18.4-20.8 | 0.2 / 17 / 33 ms |
| interpreter, host GPU | installed | PASS | 287 s | 17.0-20.6 (18.8 typical) | 0.2-0.3 / 21-23 / 30-31 ms |

Reading: with the host GPU Let's Golf 2 runs at about 19 fps (the game's menu and intro screens) on both CPU engines; the guest-code share per frame (~30 ms) is the same for the JIT and
the interpreter, so on this workload the frame time is not the ARM CPU: the swap is `post` = the window server's software rotation (see the Sparkle section in `sparkle-gl-run.md`). The JIT reached its
first frame later than the interpreter (366 s vs 287 s) on the same installed disk: translation cost at start-up; one run each, not repeated, so the 80 s difference is a single measurement. The 19 fps is the intro/menu
(title, Input Name); a playable hole was not driven in these runs (no input sent), so in-game fps is not measured.
Software pipe earlier (compat.md, 2026-11-04): 3-11 fps interpreted in the hole; the menu numbers here are not comparable to it.

Evidence lines (`serial-*.log` of the runs):
- `[symgl] eglInitialize: svga (VMware SVGA3D, host GPU) screen up` (all three runs)
- `[symgl] scan-out: not full-screen (window 640x360 at 0,0, display 360x640)` (all three: the same rotation-bound path as Sparkle)
- `proteus cpu: jit` / `proteus cpu: interpreter`
- JIT: `GLSTAT frames 2520: 19.8 fps over 1513 ms; per frame 50.4 ms = GL calls 0.2 + present 17.2 + rest (interpreter, services) 33.0`; `[symgl] prof/frame2: ... 600 swaps in 30292 ms = 19 fps`
- interpreter: `GLSTAT frames 2580: 18.8 fps over 1592 ms; per frame 53.0 ms = GL calls 0.3 + present 22.5 + rest 30.2`; `600 swaps in 30725 ms = 19 fps`
- `P3 LetsGolf2.exe: create 0 exit type 3 reason 0 (running after 240 s)`, `P3 window group ... 2003b2cb Lets Golf 2`, `P3 (installed ARM app) PASS`; the label "rest (interpreter, services)" in GLSTAT is generic, it is also printed under the JIT.

Screenshots: `img/compat-letsgolf2-3d-jit-20261107.png` (fresh-disk JIT run: Input Name), `img/compat-letsgolf2-3d-jit-installed-20261107.png` (installed-disk JIT run), `img/compat-letsgolf2-3d-interp-20261107.png`
(interpreter: title "Touch to continue"). The screens differ only because the shot is taken at a fixed time after the first frame and the intro clips have different lengths of work in front of it.

Not done in this session (time): the Calculator and PyS60 on the host GPU, a hole of Let's Golf 2 (needs input), and repeats of the first-frame time.

## Native on the PinePhone (2026-11-07)

The same two games, run natively (ARM, on the phone's own kernel, 4 x A53) from E:\sys\bin with `phestart <uid>`
(Sparkle 20030f70, Let's Golf 2 2003b2cb), image already on the phone (boot 2354, slot B), volume turned down 15 times.

**The `[symgl] prof/frame` / `prof/frame2` / `eglInitialize` lines do not exist on the phone.** That stack (symgl + Mesa,
used by the PC image and Proteus) is not what the phone runs: the phone's native GL is the `libGLES_CM.dll` /
`libGLESv2.dll` wrapper behind the Alfred window server (`PH: alf: wrapper eglInitialize done`), with its own drawing path
(`alf: RI gpu path fills`). No renderer string and no per-600-swap profile is printed, so a like-for-like number from
Mesa's lines is not available. What the phone does print is the kernel display counter, `[EKERN][frame] active second:
frames=N` (frames the display engine scanned out in the last busy second). That is the figure below, so it is a
presented-frame rate, not the game's own swap count, and it is not the same instrument as the PC column.

| game | screen | phone (presented frames/s) | PC, host GPU (`prof/frame2`) | notes |
|---|---|---|---|---|
| Sparkle (Qt + GLES 1), story/title screen | 640x360 landscape | 48-53 (0x30-0x35, many samples) | ~62 | main thread ~10% of one core, whole phone ~5% busy; compose update->composed 7.1-7.4 ms |
| Let's Golf 2, Gameloft logo video | 640x360 | 29-30 | (video) | the intro video's own rate |
| Let's Golf 2, main menu (3D golfer animated) | 640x360 | **10 steady** (0x0a for 30+ consecutive samples; display update period 62-69 ms) | ~19-20 | update->composed 10-11 ms |

Findings:
- Let's Golf 2 is **not** at a 20 fps cap on the phone: it presents a flat 10 fps in the menu, half the PC figure, so the PC
  ~20 fps (50 ms frames) is the game's own cap and the phone currently runs at half of it. Not investigated further
  (gameplay was not reached; only the menu was measured, within the 60-minute budget).
- Sparkle on the phone is ~50 fps at the title screen, close to but below the PC's ~62. The scene is static-ish text and a
  button, so this is an upper bound for gameplay.
- Per-frame breakdown (GL / present / guest) is not available natively; the only phone-side timing is the compositor's
  update->composed average (7.2 ms Sparkle, 10-11 ms Golf menu).
- The screen blanks about 10 s after the last input even with a game in front, so each sample window needed a power-key wake.
  The screen recorder (`record_start`) captures at its own 30 fps and burns ~54% of a core in `pherec`; it cannot be used to
  count game frames.
- To get a real swap-based number natively, the phone's `libGLES_CM` wrapper needs a swap counter printed like symgl's
  `prof/frame2` (not done).

Screenshots: `img/phone-sparkle-20261107.png` (Sparkle title, landscape rotated on the portrait panel),
`img/phone-letsgolf2-20261107.png` (Let's Golf 2 main menu).

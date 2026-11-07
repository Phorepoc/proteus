# Sparkle (a Qt + OpenGL ES 1 game) under Proteus, with native GL

Run: PC image in QEMU (TCG), mesa-symbian softpipe, core's symgl bitmap fallback (no SurfaceManager). `tests/board_boot.py --sis`.

## Result

Sparkle installs, runs through its Qt start-up, initialises OpenGL ES (`eglCreateWindowSurface` on the guest's window,
context, GLES1 calls all native) and its sound (MMF), loads its data and **draws its loading screen**
(`eglSwapBuffers` returns 1; the frame reaches the window through symgl's bitmap path). It is slow: the interpreter and
softpipe both run under TCG, a few frames in several minutes.

## What had to be fixed to get there

1. `EExecMsgQueueCreate` and the other RMsgQueue executive calls were not emulated: the guest halted in the MMF sound
   init. They are now (local queues, `proteusexec.cpp`).
2. A server can hand a client a **session handle in a reply** (the MMF audio server does: `RMessage2::Complete(handle)`).
   That is a kernel handle of the proteus process, outside every range the guest's own handles use. The bridge answered
   KErrBadHandle (-8) and Sparkle's sound init left. The bridge now adopts such a handle at its first use as a bridge
   session (`NativeLooking`, `AdoptNative` in `proteus.cpp`). The guest's own handle ranges are 0xC0..0xF7 in the
   instance field (chunks, sync objects and queues, bridge sessions, properties, loader, libraries, threads).
3. Sparkle paints from the paint event: it is inside its own `RWindow::BeginRedraw` when it calls `eglSwapBuffers`.
   symgl's bitmap fallback did `Invalidate / BeginRedraw / BitBlt / EndRedraw` on the same window, a nested BeginRedraw,
   so WSERV panicked the app (WSERV 69, EWservPanicDrawCommandsInvalidState). The fix is in core
   (`os/src/symgl/symgl_window.cpp`, PresentBitmap): no bracket of its own. (Patch kept with the run scripts, not yet
   in core's tree.)

## Open

- (done, see Landscape below) Sparkle is a landscape game (GL viewport 640x360): the PC image has a landscape screen mode now.
- Speed: see "Interpreter speed" below (VirtualBox 150-200 MIPS; QEMU TCG 15-17).
- P3's verdict is at the end of the 240 s wait; the screendumps (`--linger`, `--shot-at`) provide the display refresh
  Sparkle waits for under `-display none`.

## A/B evidence: a fresh build directory seeded from the CI image restarts under SSM at about 23 s

A ROM built in a build directory copied from `ghcr.io/phorepoc/core-x32:main`'s `/build` (volume `protgl-pc`) boots to
the window server, then `aknnfysrv` ends with 0 and the kernel logs `[power] restart` (QEMU `-no-reboot` exits). No
SysAp, no `Menu`, no `Main` application threads in a thread dump taken at 14 s guest time (DUMP=14). Same sources, a
copy of the older working directory `prot-pc` (`protgl-pc2`): boots, SysAp at 15 s, everything above runs. Excluded by
A/B on the bad directory: GL in the ROM (a ROM without it restarts the same), core at d18ee94 (restarts), pdk at
24b64e8c (restarts), screen scale (SCALE=2 restarts). The ROM's file list is the same as the working one's; the
difference is in the build directory's binaries or data. Not chased further.

## ViewSrv 11 / time slicing (2026-11-04)

**Which thread is busy.** With per-thread instruction counts in the progress line (`cur=0 ran(M): 0:1275 1:0 2:4` at 335 s), the
busy thread is thread 0, **the main thread**, running Sparkle's Qt start-up and data load (`qtgui`, `qtcore`, `User::Alloc`). The Qt
threads (thread 1 `qtcore+0xe633c`, thread 2 `qtcore+0xe7578`, both created by thread 0) are idle: 0 and 4 M instructions. So the main
thread's active scheduler is not starved by another thread; the main thread itself does not return to it. Preemption cannot make
that thread service its own scheduler; only a faster interpreter, or the load yielding (Qt's processEvents), can.

**What is in the tree.** Time slicing (`CArmCpu::iOnSlice`, `Preempt` in proteusexec.cpp: native request completions taken without
blocking through `TIpcBridge::iPoll` = `RThread::RequestCount`, timed waits expire, round robin to the next ready thread; the exclusive
monitor of LDREX/STREX is real and is cleared by a thread switch) is **on** (`KSliceInstructions` = 1 << 17).

## Stray signals: root cause and fix (2026-11-04)

With slicing on, signals are credited between the threads at times the cooperative scheduler never reached, and Sparkle's Qt threads died
of a stray signal (E32USER-CBase 46 at 84.7 s). `AttributeNative` credited 30+ native signals "by guess" (`native signal with no completed
noted request`). The log now says, for every signal, which noted request explains it (`sig note` when a guest request goes native,
`sig credit` when a signal is credited to it, `sig credit: owed`, and `PH: proteus sig:` for each native source in proteus.cpp), and the
unexplained ones dump the whole noted table. Two causes, both of the bridge's own making:

1. **Synchronous sends completed by the bridge** (`CompleteGuest`: a SessionSendSync, a connect, the file-server private-path
   answers) wrote the guest's status and then called `RThread().RequestSignal()` on the *native* thread. The signal was anonymous: it was
   matched with its status later, when the native count was next taken, and by then the guest thread had reused the same stack
   `TRequestStatus` for its next call (one entry per status address in the noted table) -- two signals, one entry, one signal unexplained.
   Now the bridge only reports the completion (`TIpcBridge::iSyncDone`) and the executive credits the sending thread at once
   (`CompleteFor`); no native signal exists for it.
2. **A status reused before its signal was credited.** Qt's `QEventDispatcherSymbian` runs a completed active object with
   `CActiveScheduler::RunIfReady`, which does not take the request signal, and the object issues its next request on the same
   `iStatus`. The earlier request's signal (an asynchronous window-server or CenRep send completing natively) is still owed. A new note
   for a status that still has an open entry now marks that entry *owed* (`iReqDone`) and adds the new one beside it; an owed entry takes
   the next signal first.

Result (Sparkle, slicing on, 1 << 17): 0 unexplained signals in 360 s of guest time (was 30+ within the first 100 s); no CBase 46; P0-P3
PASS (P3 verdict at 240 s, Sparkle still running at 300 s). Real kernel semantics are unchanged: the guest still sees one request
signal per completion.

## Speed (measured, QEMU TCG, the PC image)

The interpreter runs **6.4-6.9 million ARM instructions per second** (64 M instructions per 9.3-10 s of wall time, steady from 64 M to
1024 M; one QEMU at a time, no KVM). Sparkle's start-up up to the loading screen is more than 1 G instructions on the main thread (thread 0
ran 1020 M of 1024 M at 311 s; the Qt threads 1 and 2 are idle: 0 and 3 M). Samples of the pc every 64 M instructions: euser's
`__e32_atomic_add/cas_acq32` loops (Qt reference counts), `RFastLock::Wait`, `memset`/RHeap, dfpaeabi (soft float), qtcore/qtgui. A
1 GHz ARM11 retires several hundred million instructions per second, so the same load takes a few seconds on the device: the main thread
never starves its active scheduler there, and ViewSrv 11 does not arise. Slicing cannot help a thread that does not return to its own
scheduler; the interpreter's speed is the lever: **next step = interpreter speed** (target 50+ MIPS: the load then fits the view server's
20 s), starting from a real profile (perf on proteus.exe under the host, not pc samples), with the dispatch loop (decode cache, flag
laziness), the memory access path (`Valid` + window scan), and euser's atomics (HLE them as one host operation) as candidates.

## Landscape

The PC image has a portrait mode (360x640) and a landscape one (640x360, rotated 90 degrees, software rotation in the OMAP screen driver).
Sparkle asks for landscape (`SetOrientationL`) and gets mode 1 while it is on top (sgc log: `top wg 35 -> mode 1`), uncropped; the home screen
returns to mode 0 when it leaves (`top wg 36 -> mode 0`).

## Interpreter speed (2026-11-04)

**Method.** A host benchmark first, not QEMU: `tests/bench` (`bench.c` = a freestanding ARM guest with the shapes the Sparkle load spends its
instructions in: LDREX/STREX loops (Qt reference counts), memset/memcpy byte loops, a free-list walk, virtual calls, string compares, a
software float (CLZ + UMULL normalisation, what dfpaeabi does), an ALU mix; `bench.cpp` loads it into `CArmCpu`; `run.sh`, `THUMB=1` builds
the same workload as Thumb-1; the guest prints a checksum that must not change). Three yardsticks: the host (native), `qemu-x86_64` user-mode
TCG (a proxy for "interpreter under TCG"; no softmmu, so it flatters the real thing), and the real PC image in QEMU system mode and VirtualBox
(the `proteus progress` lines). The host profile came from callgrind (`valgrind --tool=callgrind`), per source line.

**Profile before (callgrind, 9.7 M guest instructions): 196 host instructions per guest instruction.** `Step()` 40% (the per-instruction
breakpoint loop, trap/foreign-pc checks, then ~12 mask compares in a chain to find the instruction class: every ALU instruction walked through
the BX/CLZ/MLS/UMAAL/multiply/halfword/LDREX/STREX/DSP/QADD/MRS/MSR tests before `DataProc`), `DataProc` 13%, `SingleTransfer` 13%, `Run` 7%
(progress and slice checks after every instruction), `BlockTransfer` 5% (a 16-iteration popcount loop), `Operand2` 5%, `Write32`/`Read32`
(`Valid` = region check + page-map loop + window scan) 7%, `Cond` (a switch) 3%. Registers, flags, pc and the step counter lived in the object,
so every instruction was a dozen loads and stores of interpreter state.

**Changes** (`armfast.inc`, included at the end of `armcpu.cpp`; `Step()` and `thumb.cpp` stay as the generic path, unchanged):
1. One table lookup classifies an instruction: ins bits 27:20 and 7:4 index a 4096-byte class table. Data processing carries op, S and operand
   kind in the class (96 template instantiations with the operation constant-folded), LDR/STR carry P/U/B/W/L and register offset (64), plus LDM/STM,
   B/BL/BX/BLX, MUL/MLA, long multiplies, halfword, LDREX/STREX, MOVW/MOVT, media. Everything else (VFP, SWI, the misc group, cond 0xF, traps,
   breakpoints, a pc outside the region) goes to `Step()` as before.
2. `pc`, the flags (one NZCV nibble = the index of a 16-entry condition mask table, nothing at all for AL), the step counter and the region
   description are locals for a whole batch; the batch runs to the next progress / slice / budget boundary. `Run` no longer does per-instruction
   bookkeeping. No `FS` address escapes into a call (the slow memory path takes the pieces), or gcc keeps it in memory.
3. Guest loads and stores hit the region directly (one range compare + one page-map byte); windows, page-crossing accesses, faults and
   watchpoints fall back to the exact `Read32/Write32`. The fallback count is in the progress line (`mem=`): ~19 K per 64 M instructions,
   so this is not where the time goes.
4. Thumb-1 runs in the same batch loop (ARM <-> Thumb switches are a flag, not a return to `Run`). The Sparkle executable itself is Thumb (its
   progress pcs are all 2 mod 4): before, every one of its instructions went through `Step` -> `ThumbStep` -> `Read16` -> `Valid`.
5. LDM/STM use a bit-trick popcount + `ctz` walk (libgcc's `__popcountdi2` is a call on the x32 target); `__builtin_memcpy` for the fetch (the
   x32 build is `-fno-builtin`, so a plain `memcpy` there is a libc call).

**Tests (all green).** `tests/isa/run.sh` runs three differential tests, `qemu-arm` against the interpreter, identical output = PASS: `isa.c`
(ARMv6/v7 additions, 4803 results), **new** `base.c` (all 16 data-processing ops x S x 17 operand forms with flags in and out, LDR/STR/B/H/SB/SH in
every addressing mode, LDM/STM four modes +/- write-back, every condition over every NZCV, MUL/MLA/long, BX/BLX/BL, ARM<->Thumb round trip,
LDREX/STREX B/H/W, MOVW/MOVT, pc as an operand; 4038 results) and **new** `thumb.c` (every Thumb-1 format entered and left by BX, flags captured
without MRS; 395 results). `base.c` and `thumb.c` also pass on the pre-change interpreter (so they test the reference, not the new code). Known
deliberate difference, left out of the tests: an unaligned LDR rotates (ARMv5), ARMv7 does not.

**Numbers (MIPS; the same bench checksum before and after).**

| yardstick | before | after |
|---|---|---|
| host native, ARM workload | 157 | 436-474 |
| host native, Thumb workload | 199 | 535 |
| qemu-user TCG, ARM workload | 14.6 | 48.1 |
| qemu-user TCG, Thumb workload | 19.5 | 62.5 |
| PC image in QEMU system mode (TCG), Sparkle ARM phases | 6.4-6.9 | 15-17 |
| PC image in QEMU system mode, Sparkle's Thumb exe phase | 7.1 | 16-18 |
| PC image in VirtualBox (hardware virtualisation), Sparkle | not measured | **150-200** (3.5 G instructions in 22 s; 22.5 G by 355 s) |

Host instructions per guest instruction: 196 -> about 85 (callgrind). **The 50 MIPS goal is met on VirtualBox (the image the public runs) by
3-4x and not met on QEMU TCG (15-17).** QEMU system mode costs ~3x more than the user-mode proxy for the same code (every load and store of the
interpreter's own data goes through the softmmu TLB; the loop still touches ~10 words of state per guest instruction), and a table-driven
interpreter cannot get much below that: the next step on QEMU is a translation cache (basic blocks of pre-decoded instructions, registers
cached across a block), i.e. the JIT the brief put out of scope until numbers were in. These are the numbers.

**What else the VirtualBox run found (it is fast enough to get there).**
- `User::AfterHighRes` (slow exec 135) was not emulated: Sparkle halted at it right after "Pushing layer Loading" (221 M instructions in). Added
  `Exec::After` / `Exec::AfterHighRes`: the thread's own timer (`DThread::iTimer`) completes the `TRequestStatus` the exec is given at its
  deadline (`FireAfter` in `proteusexec.cpp`, checked at every slice end and when all threads are blocked; the scheduler's native wait is
  bounded by the nearest timer).
- A 4 G instruction cap (`RunExe` took a `uint32_t` budget; the board passed `0xFFFFFFFF`) stopped Sparkle with `EStepLimit` once the interpreter
  was fast enough to reach it. The budget is `uint64_t` and unlimited for a real app; `Run` limit arithmetic is overflow-safe.
- With both fixed, Sparkle on VirtualBox runs past its whole data load (22 G instructions by 355 s) and reaches `GameWidget::startAnimating`.
  Two things stop it from reaching the menu: (1) an active object leaves **-4 (KErrNoMemory)** while caching the sound effects ("..caching sound
  'sfx/throw-orb'"): the guest heap or the region is exhausted (check Sparkle's heap max against the device default, `tools/heap_audit.py`
  style, before anything else); (2) the VirtualBox screen shows a sheared copy of the loading screen (360x640 portrait window, 640x360
  landscape content): the landscape mode switch seen on QEMU does not reach the VMSVGA display. Neither is the interpreter. Screenshot:
  `docs/img/sparkle-vbox-sheared.png`.
- P0-P3 PASS on the final build in QEMU (P3 = the installed Sparkle starts). ViewSrv 11 did not occur in any run since the speed-up (QEMU: the
  load needs more than 2 G instructions at 4 s per 64 M).

**Progress-line statistics** (new, `proteus progress`): `wait` (ms blocked natively in the scheduler), `slow` (instructions that left the fast loop for
`Step`), `svc`, `trap`, `mem` (guest accesses that left the region fast path), all in K. They showed that the QEMU phase that looks like "50 s for
64 M instructions" (192 M -> 256 M, `dfpaeabi`) is not interpretation: `wait=0`, 11 K SVCs, 0.65 M slow steps; it is the native image decoders and
the emulated ATA disk loading Sparkle PNGs.

**Open (speed).** QEMU TCG: a block cache / JIT (see above). VirtualBox: nothing in the interpreter; 150-200 MIPS leaves a Sparkle start well
inside the view server limits. Sampling the guest with the QEMU monitor (`info registers -a` in a loop) is cheap and needs no instrumentation
in the guest; it only shows kernel idle when the interesting thread is blocked, so take the samples inside an interpretive window.

Final build on QEMU (P0-P3 PASS): with `AfterHighRes` working Sparkle sleeps and polls through SVCs (`svc=` 118 K per 64 M instructions after the
load), so the QEMU rate in that phase is ~10 MIPS (64 M per 6-8 s) and 1.47 G instructions at 357 s; the 15-17 MIPS figures above are the
interpretive phases before it.

## The two blockers, solved (2026-11-04, VirtualBox)

**1. KErrNoMemory (-4) was the sync object table, not memory.** The 512-entry table of mutexes, semaphores, condition variables and message
queues (`KMaxSync`, `TExecState::iSync`) had no way to free a slot: `EExecHandleClose` of such a handle was a no-op and every create took
`iSyncCount++`. Qt creates and closes these per object, so the 513th `EExecCondVarCreate` answered -4 and the active object that cached
`sfx/throw-orb` left with it. Found by logging every -4 an executive call or a bridge call returns (they are now always logged, bounded:
`exec EExecCondVarCreate -> -4 (caller euser.dll RCondVar::CreateLocal ...)`). The guest heap was not the limit: Sparkle's header says heap min 128 KB,
max 128 MB (0x8000000), the real euser asks for the 256 MB chunk it asked for before, and Proteus gives exactly that (the guest region is 1 GB,
reserved; the PC image's RAM is not touched until pages are committed). Nothing was bumped: Close frees the slot (handle count), a free slot is
reused, and the table grows (realloc) up to what a handle can name (`KMaxSync` = 0xF0000: KHandleObj + index must stay below the bridge handles).
`Duplicate` of these handles is still not emulated (no one asks). Other fixed tables with the same shape, not hit yet: guest chunks (64, never
freed), threads (32), logons (8).
Result: the data load completes on VirtualBox (about 4.8 G instructions to the Start button), the loading bar fills and the **Start** button appears
(`docs/img/sparkle-vbox-start.png`); a tap on it (VirtualBox mouse integration) gives "Clicked 'start'", the music switches to main-theme and the first
scene renders (`docs/img/sparkle-vbox-map.png`: mushrooms, tower, windmill; a landscape picture on the portrait panel, rotated 90 degrees).
Note: the PC image blanks the screen lights after ~30 s without input and the focus then goes (Sparkle: "Focus lost -> BackgroundStop", it stops animating);
a key press (Ctrl) wakes it. A run on VirtualBox that waits for the menu has to send a key every 20 s.

**2. The sheared screen was the screen driver's pitch, on every screen, not only landscape.** `docs/img/sparkle-vbox-sheared.png`,
`docs/img/landtest-vbox-sheared.png`. Cause: the OMAP draw devices (used since adaptation-pc v0.3.0 for the software-rotated landscape mode)
read the row pitch from the HAL and then call the base class's `Construct(aSize)`, which sets the pitch to exactly width * 4 again. The PC
display pads rows to 64 bytes like the 701 (1472 bytes for 360 px; applications writing screen memory assume it). In VirtualBox VMSVGA the screen is
always the padded shadow screen, so WSERV drew 1440 bytes per row into 1472 byte rows: the whole screen sheared (the home screen too, in portrait;
the v0.3.0 notes only showed landscape). QEMU draws straight into a framebuffer of exactly 1440 bytes per row, where the two agree, so every QEMU test
passed. Confirmed three ways: an unpadded shadow (ScreenWpl = width) in VirtualBox drew correctly; the fix below draws correctly; the pattern of the
new test is identical on both. Fix (adaptation-pc 0298654, pushed): `os/src/phescdv/scnew.cpp`, the driver's factory, builds the 32-bit
draw devices through a subclass that puts the HAL's pitch back after the base class has run; test `phelandtest` (SSM_TEST: landscape mode + a
pattern with corner marks, ramp bars, a frame and a diagonal). Checked: phelandtest on QEMU and VirtualBox (`docs/img/landtest-vbox-fixed.png`, same
picture), the portrait home screen on VirtualBox (`docs/img/home-vbox-portrait-fixed.png`), all 17 apps of the regress list ALIVE in QEMU portrait.
The 8- and 16-bit OMAP classes have the same flaw and are unused here (the display is 32 bits).
Sparkle after the fix, VirtualBox: `docs/img/sparkle-vbox-pitch-fixed.png`.

## Playable on VirtualBox (2026-11-04)

Setup: the PC image (adaptation-pc 0298654, the pitch fix), VirtualBox 2 CPUs, 1 GB, VMSVGA, AC97, `SSM_TEST=proteus` ROM (so P0-P3 run and P3 starts the
installed Sparkle), a C: with Sparkle installed (the SIS removed after the install). Input through the VirtualBox COM API (`PutMouseEventAbsolute` =
mouse integration; `tools/phorepoc-x86/vbox_mouse.ps1` of the main repo). Screenshots (`VBoxManage controlvm screenshotpng`) are in `docs/img/sparkle-vbox-*.png`.

**It plays.** Cold boot to the Start button takes about 3.5 minutes (4.8 G guest instructions), then: Start -> main menu (animated, particles) ->
Play -> Play Quest -> Instructions -> OK -> the game (orbs run along the path, taps throw orbs, orbs match and burst, lives, "level failed", back to
the menu, "About lives" dialog). Several sessions of 7-10 minutes of play with a tap every 5 s, no crash, no halt, no unemulated exec, no `-4`.
Touch: panel pixel = game pixel in portrait (the game screen), and in the landscape screens (menus, dialogs: 640x360 drawn rotated on the 360x640 panel)
the taps hit the right buttons too (Start, Play, Play Quest, OK, "OK" of About lives) -- WSERV maps pointer events to the screen mode, nothing in
Proteus is involved. Note the game itself switches the screen mode: menus and dialogs are landscape, the level is portrait.

### Display lights: the power management is the PC image's, Sparkle does not fight it

SysAp's inactivity timer turns the lights off after ~30 s without input (`HandleInactiveEvent` -> `SetLightsOffL`), and the foreground app then loses focus
(`Focus lost` -> `moveToBackground` -> `stopAnimating` -> `BackgroundStop`; the frame counter stops). A key press wakes the screen and Sparkle resumes
(`moveToForeground`, `startAnimating`) -- both directions seen, `GLSTAT` count frozen 103 -> 103 for the idle minute. Sparkle never calls
`User::ResetInactivityTime`: across all runs no `EExecResetInactivityTime` / `EExecUserInactivityTime` reached the executive (it would have halted the guest
as "not emulated"). Both are now emulated by forwarding to the native kernel (`BridgeKernel` in proteus.cpp: `User::ResetInactivityTime()` and
`User::InactivityTime()`; the inactivity time is the system's, so a game that resets it from its frame loop keeps the display on exactly as on a device).
That path is **not exercised by Sparkle** (no ARM test program calls it either), so it is untested beyond compiling. For test runs the keep-alive is a
Ctrl key press every 15 s (a modifier alone wakes the lights and does nothing to the app); the proper CenRep setting (display lights timeout, repository
10200C8C key 6) would be a C: file change -- not needed.

### Sound: end to end, three things were missing

The MMF path is the A3F DevSound: the guest's ARM `mediaclientaudio` / `mmfdevsoundproxy` talk to the native `!MMFAudioServer`, whose DevSound session
(a session handle handed back in a reply -- adopted already) runs `EMMFDevSoundProxyInitialize4` with **a message queue handle** in an IPC argument and later
replies with **a chunk handle** (the shared buffer). Before: the queue was a guest-local object, the server's `RMsgQueue::Open(aMessage, 2)` answered
KErrBadHandle (-8) ("native session handle fn 4 -> -8"), `SoundImp initialized` was logged but nothing played; with the queue fixed the next reply
handed a chunk handle the guest then used with `RChunk::Base()` -> 0 -> `memcpy` write fault in `mediaclientaudiostream` (guest halted `write32`).
Fixes (proteus.cpp, proteusexec.cpp):
1. **Message queues are real kernel queues of the handler process.** `EExecMsgQueueCreate` (local and global) creates a native `RMsgQueueBase`; Send, Receive,
   NotifySpaceAvailable / NotifyDataAvailable (noted for the signal accounting like timers), the Cancels and Size are the native calls on it. The handle
   is a bridge object handle. A queue the embedder does not own (the host test has no native side) stays the old local emulation.
2. **Handle arguments are translated.** `TIpcArgs` types an argument `EHandle`; the real kernel resolves that in the sender's table. `BridgeSend` replaces an
   `EHandle` argument that is one of the guest's bridge object handles (the queue, a chunk opened by name) by the object's real handle in a copy of the
   `TIpcArgs`. (Guest-local handles -- chunks, mutexes, semaphores of the emulation -- cannot be passed to a native server; not needed so far.)
3. **A kernel object handed over in a reply is adopted at its first use as its type** (`AdoptObj`; sessions already were): a chunk (`EExecChunkBase/Size/MaxSize/
   Bottom/Top` on a handle outside the guest's own ranges) is wrapped with `SetHandle`, the guest keeps the handle's value, and its memory becomes a guest
   window (`proteus kernel: adopted chunk handle 6ac60019: window 70004000 + 3000`). Close finds it by value. Other types adopt the same way when one is needed.
Result: the audio reaches the device. VirtualBox's own statistics (`VBoxManage debugvm <vm> statistics --pattern "/Devices/ichac97/Stream1/*"`): the AC97
PCM-out stream is started and read by the device, `offRead` 8.9 MB (~46 s of 48 kHz stereo) after a minute of play, and a WasAPI output stream is created at
the host. **Not verified by ear** (headless run, nobody listening): that the samples are the game's music and not silence. A person at the VM has to
confirm it.

### Frame rate and where the frame time goes

`eglSwapBuffers` is counted in the guest GL trace (`proteusgl.cpp`): every 30 swaps a `GLSTAT` line gives the rate over the last interval and the split of one
frame into native GL calls, the present (native `eglSwapBuffers`: symgl's bitmap copy + WSERV) and the rest (the interpreter + other native services).
Each call is timed with the kernel fast counter, extended to 64 bits at every sample (it is 32 bits and wraps in seconds on this PC); the sums are scaled by the
wall clock from the tick count (the HAL's counter frequency is not the PC's real rate; using it made GL "take" more than the frame). VirtualBox, 2 CPUs:

| phase | fps | frame | native GL (softpipe) | present | rest = interpreter + services |
|---|---|---|---|---|---|
| loading screen, menus (first 30 lines) | 14.4 | 101 ms | 59.6 (59%) | 8.6 | 32.8 |
| level, first minutes | 17.4 | 63 ms | 41.7 (66%) | 8.6 | 13.0 |
| level / dialogs, later (43 lines) | 13.0 | 82 ms | 54.2 (66%) | 11.3 | 16.6 |

So in the game two thirds of a frame is the native software rasteriser (Mesa softpipe: the PC image has no GPU path), the present is ~10 ms and the
interpreter and all native services together ~13-17 ms (~20%). The interpreter is no longer the limit on VirtualBox; the next gain is the GL side (softpipe on
one core: fewer / cheaper draws, or the llvmpipe-style JIT rasteriser, or SVGA3D in the VM). Frames per second are 13-17, the game is playable, not smooth.

### Other findings

- The `Duplicate` of sync-object handles and the fixed guest tables (chunks 64, threads 32, logons 8) were not hit in 10-minute sessions (no `no slot` / `no window slot`
  lines); the adoption of non-chunk handle types is written for sessions and chunks only.
- A fresh `docker run` that sits in `Created` (Docker Desktop hung on a bind mount) needed `docker rm` + a Docker Desktop restart; a bind-mounted disk image cannot be
  used by QEMU (write lock) -- copy it into the container first.

### Regression checks (same day)

- **VirtualBox app regression** on the normal ROM built with the Proteus layer (`phe image rom -O prune=1 -O leave-trace=1`, `/w/proteus` in
  `PHOREPOC_ADAPTATIONS`, blank C:, the 17 apps of the regress list started one by one by bootui, 20 s each): **17 / 17 ALIVE, no panic, portrait**
  (`docs/img/regress-vbox-portrait-gsapp.png`). Native apps do not touch the handler; this shows the ROM with the new handler is unharmed.
- **QEMU** (TCG, the same `SSM_TEST=proteus` ISO as the VirtualBox runs): **P0, P1, P2, P2b, P2c, P2d, P2f, P3 PASS** (440 s; the native queue
  path runs there too: `EExecMsgQueueNotifyDataAvailable` on the native handle, signal credited).

### Music missing: not the mixer, not an underrun (2026-11-04, audio A0 measurement)
**Resolved the same day: the next section (the MMF controller hosted natively). Point 3 below named the cause; the fix is not ARM twins of the chain.**

Symptom: effects play, Sparkle's music (MP3 files, `music/main-theme`, ...) is nearly absent. Measured on the PC image in VirtualBox (AC97 via the
null audio driver with VirtualBox's own WAV capture, `VBoxInternal2/Audio/Debug/Enabled`), with the audio trace switches on (`PHOREPOC_AUDIO_A0`, and the
mixer's new one-line-a-second summary, `PHMIX_TRACE`).

1. **Sparkle uses two audio paths.** Effects: its own software mixer into one `CMdaAudioOutputStream` (22050 Hz mono, 1024 B writes). Music: `CMdaAudioPlayerUtility`
   on the MP3 files (the game imports `mediaclientaudio`). Only the first reaches the audio server: there is one channel on the mixer, never a second.
2. **The stream path carries silence between effects**: over 149 s of play after "Switching the music" every one-second mixer summary has `peak 0`; the VirtualBox
   capture of 560 s has audible samples in 3 seconds. The mixer is not dropping anything (it mixes what it gets) and MMRC never pauses or denies: one stream.
3. **The music open fails in the guest.** Right after `Switching the music to 'music/main-theme'` the guest connects to `!ecomserver`, gets -17004 twice,
   and `proteus DLL f32agent.dll: no ARM file (stays HLE)` / `RLibrary::Load Z:\sys\bin\f32agent.dll -> -1`. The MMF client of `CMdaAudioPlayerUtility` loads
   the controller **plug-in into the application's own process**, so the controller chain must exist as ARM twins in `c:\sys\bin\arm`: `f32agent` (CAF),
   `mmfaudiocontroller` / `mp3audioplaycontrollerplugin` (+ recogniser, `mp3audiocontrollerutility`, `arimp3decmmfcodec`, `arimp3decwrapper`, `mp3hwdevice`)
   -- the names the native ECOM registry answers with. None is in the Sparkle closure (273 modules), so the open fails and the game goes on without music. This is a
   closure gap (a staging task: ARM builds of those components, then `proteus_closure.py --extra`), not a DevSound buffer size: the stream's `PlayData`
   size is the game's own 1024 B and the buffer chunk the server hands out is 12 KB already.
4. **A real scheduling effect, fixed here.** The guest threads share one native thread and a native call is indivisible. The audio thread (guest thread 2) was woken by the
   native reply only at the end of the current slice, which a long GL call (softpipe, tens of ms) stretches: the effects stream ran at ~13 KB/s of the 44 KB/s it needs
   (turnaround completion -> next write: median 28-38 ms, p90 110-220 ms, in the traced run). A native GL call that takes long now ends the slice, so the scheduler polls the kernel
   and runs the woken thread at once; wake-up latency is counted per thread (`wakeN=count/avg us/max us/over 10 ms` in the progress line): thread 2 20120 wakes, avg 47 us,
   28 over 10 ms; the stream then carries ~29 KB/s on average. Guest thread priorities are honoured since the "priorities" commit (compat.md, "Guest thread priorities").

### Music: the MMF controller runs natively, behind its own private server (2026-11-04)

Decision (user): no ARM copy of the playback chain (slow interpreted decoders, duplicate binaries). The MMF architecture has a boundary of its own for this, and
Proteus uses it: the **controller thread and the session to it**. Everything below was read from the 2010 source
(`oss.FCL.sf.os.mmaudio/mmlibs/mmfw/src/ControllerFramework/MMFControllerFramework.cpp`, `mmfcontrollerserver.cpp`, `.../Client/Utility/mmfclientutility.cpp`) and
`kernelhwsrv/kernel/eka/euser/us_ksvr.cpp`.

**The protocol (the client's side, all in the client's process).** `CMdaAudioPlayerUtility` (and the recorder, the converter, `CVideoPlayerUtility`, the video recorder:
all five clients in `Client/Audio` and `Client/Video` go through `CMMFFindAndOpenController` -> `RMMFController::Open` -> `RMMFControllerProxy::LoadController`) does:
1. `CLogonMonitor::NewL` (an `RServer2` member, handle 0), then `DoCreateSubThread`: `RThread::Create(KNullDesC, &CMMFControllerProxyServer::StartThread, stack, ..., &params)` with
   `params = { RServer2* iServer; TBool iUsingSharedHeap }`, `Rendezvous(status)`, `Logon` (the monitor), `SetPriority`, `Resume`, `User::WaitForRequest(rendezvous)`.
   `StartThread` renames the thread `MMFControllerProxyServer-<id>`, creates the server (`CMmfIpcServer`, `EGlobalSharableSessions`), writes its handle into `*params->iServer`
   (`CMMFControllerProxyServer::ConstructL`), rendezvous.
2. `RSessionBase::CreateSession(RServer2, TVersion(7,1,1), slots)` = `Exec::SessionCreateFromHandle(server handle, ...)` + the EConnect message.
3. `SendSync(destination, EMMFControllerProxyLoadControllerPluginByUid (0), TMMFUidPckg, KNullDesC8)`: the controller plug-in (ECOM) is created **inside that thread**.
4. From then on messages only: `AddDataSource` / `Prime` / `Play` / `GetPosition` ... (function numbers of `EMMFController*`, destination = `TMMFMessageDestination` pckg in arg 0),
   `ReceiveEvents` (an async request the controller completes when it has an event: callbacks are IPC completions, nothing calls back into the client).
5. `Close`: close the session (the server shuts down), `Logon` on the thread, wait for its exit (20 s, then Kill).
(`LoadControllerInSecureDRMProcess` -- protected content, a client without DRM -- already is out-of-process IPC to `mmfdrmpluginserver`; it needs nothing.)

**What Proteus does** (`proteus.cpp` "MMF controller threads", `proteusexec.cpp`, `proteusrt.h` `TIpcBridge::iHost*`):
- `EExecThreadCreate` whose entry function lies in the guest's `mmfcontrollerframework.dll` is **claimed by the bridge** (`iHostThread`): a *natively hosted thread*, never scheduled,
  never ARM code. (`StartThread` is not exported, so the module of the entry is the key; `mmfcontrollerframework` creates no other thread.)
- Its first `Resume` (`iHostStart`) starts a native **helper thread** (own heap, cleanup stack, active scheduler) and writes a server handle (`0xE5xxxx`) into the guest's `RServer2`;
  the rendezvous logons complete (the executive, as the thread's own `Rendezvous` would).
- `EExecSessionCreateFromHandle` (new) on that handle makes a bridge session of the controller; its EConnect completes at once.
- The first message (function 0, the UID in arg 1) is handed to the helper, which calls the **native** `RMMFControllerProxy::LoadController(uid, shared)` -- the native
  mmfcontrollerframework creates the native controller thread, loads the native ECOM plug-in (MP3 controller, codec wrapper, HwDevice, CAF's f32agent) -- then `ShareAuto()` and
  completes the guest's `TRequestStatus` through `RThread::RequestComplete` on the main thread (a native request like any server's: the executive credits the signal).
  The helper owns the proxy, so the logon monitor's signals never reach the guest's request accounting.
- Every later message goes through the ordinary bridge to the proxy's own session handle, descriptors in guest memory as they are (same layout, same address space);
  `ReceiveEvents` is completed natively into the guest's status. Close of the session: the helper closes the proxy (the native `Close` waits for the controller thread),
  the guest thread ends (`iHostExit`), its logons complete with the exit reason, and its slot and stack are reused by the next controller (`KMaxThreads` is 32).
- The controller thread itself is found after the load (`*::MMFControllerProxyServer-<hex id>` of this process, opened by the id in the name) so the guest's `Kill` of it and its
  death (a panic) reach the guest: `iHostExit` polls it; `RThread::Kill` of the hosted thread kills it.
- The client ARM libraries stay what they are: `mediaclientaudio`, `mediaclientutility`, `mmfcontrollerframework` (client half), `caf`. **Two small CAF plug-ins are the exception and
  must be ARM**: `f32agent.dll` and `f32agentui.dll`. CAF's agent is loaded *by the client*, in the client's process, by design (`CMMFUtilityFileInfo`,
  `CMMFileSourceSink::EvaluateIntentL`, `UseSecureDRMProcessL` call `CData` / `CContent` before any controller exists); there is no server to move them behind. Both are built from the 2010 source
  (`phe component build -arch arm`) and staged with `proteus_closure.py --extra f32agent.dll,f32agentui.dll`; they only read the file through RFile and are not part of the playback cost
  (no decoder, no controller, no HwDevice runs as ARM).
- The controller runs in the handler process, so it has the guest's **capabilities** (the loader gives the handler the program's: an app with none cannot play a file out of
  another app's `\private\<sid>\`; its own private directory and `\Data` work).

**Test program**: `os/src/proteusmmf` (ARM EXE; `phe component build -arch arm proteusmmf`): `CMdaAudioPlayerUtility::NewFilePlayerL`, volume 1/4, `Play`, `GetPosition` every
second, `Stop`, close, a second file. Staged as `c:\sys\bin\proteusmmf.exe` + `c:\sisapp.txt` and the MP3s in its own private directory (P3 reports FAIL for it: it is not a GUI app,
there is no window group; the log lines are the result).

**Results** (VirtualBox 2 CPUs, the PC image, `SSM_TEST=proteus` ROM, AC97 on VirtualBox's debug WAV capture, `PHMIX_TRACE` through `X32_DEFS=mdasoundadapter:PHMIX_TRACE`):
- `proteusmmf`: both tracks open (`init complete 0`, duration 65848 ms / 59004 ms), play (`GetPosition` follows the clock, 1 s per second), stop, close; the second open reuses the
  guest thread slot (`thread 1` twice); the mixer shows one stereo 22050 Hz stream, peak 8000-25000, underflow 0 after the first second. Every message of the protocol above (function 0
  load, 15 / 1 / 11 / 5 / 10 / 6 / 9 / 8, async `ReceiveEvents` = 1) crossed the bridge; `async fn 1 completed -13` at close is the pending `ReceiveEvents` ending with the server.
- Sparkle: `Switching the music to 'music/main-theme'` now gives `guest thread 3 is hosted natively`, `native LoadController -> 0 (controller thread MMFControllerProxyServer-402)`, then the
  controller's messages and `RMdaDevSound ... EPlaying`: the controller opens its own DevSound stream. Level music and menu music switch controllers (close, load) without a stall; four loads
  in one session, every one found its controller thread, closed with `type 0 reason 0`, and the thread slot came back.
  Mixer, one-second summaries, **217 consecutive seconds of play in a level** (a tap every 5 s): **peak never 0** (min 11927, max 30427), both stream formats present
  (`fmt 22050/1` = Sparkle's own effects stream, 124 s, `fmt 22050/2` = the controller's stereo stream, 93 s: the second stream), 245 underflowed quanta in all (1.1 a second, 112 of the 217
  seconds had at least one; the previous measurement of the effects stream alone was 13 a second in silence). Capture: `scratchpad/shots/sparkle-music.wav` of the session (48 kHz stereo, 652 s: loading tune,
  main theme, map tune, game tunes, with the effects); screenshot of the level it was taken in: `docs/img/sparkle-vbox-music-level.png`.
- P0, P1, P2, P2b, P2c, P2d, P2f, **P3 PASS** (`Sparkle.exe` running after 240 s with a window group) on the same ISO; Sparkle's effects stream still plays (mono lines above).

**Limits, stated** (nothing is stubbed; these are properties of the design or open work):
- **A controller message is a synchronous native call**, as every bridged message is (a `SendSync` blocks the one native thread the guest runs on). `Prime` / `AddDataSource` take as long as the native
  controller takes (file open, first frames: tens of ms); the load itself runs on the helper and does not block the guest. Underflow at that moment is the effects stream's.
- **A controller thread that dies by itself** (a panic) reaches the guest when the executive next sweeps (`Preempt`, a slice, or a handle close); while every guest thread is blocked
  in `WaitForAnyRequest` nothing sweeps, and its `ReceiveEvents` (completed by the kernel with `KErrServerTerminated`) is what wakes the client.
- **Guest thread priorities** are applied to guest-scheduled threads (compat.md, "Guest thread priorities"). The hosted native controller thread is not scheduled by Proteus and keeps
  the native `EPriorityNormal` (the framework's default, `KDefaultMMFControllerThreadPriority`); the guest's `RMMFControllerProxy::SetThreadPriority` sets the guest-visible priority of its thread slot (what `RThread::Priority` answers) and nothing else.
- **Message arguments that carry handles** are not translated beyond what `BridgeSend` does for `EHandle` (queues, chunks opened by name). An `RFile` handed to the controller
  (`OpenFileL(RFile)`, `TMMFFileHandleParams`: the file server and file handles go as plain integers) would name guest-side handles; Sparkle and `proteusmmf` open by file name. Opening by `RFile`
  needs those two handles resolved to the native ones (`EHandle` arguments are; plain integers cannot be told apart) -- the next step if an app needs it.
- **Video**: `CVideoPlayerUtility` / the video recorder use exactly this path (same `CMMFFindAndOpenController`), so their controller session is bridged the same way; the PC image has no video
  controller plug-in, and the window / graphics-surface handles a video controller takes would meet the same handle limit. Not run (no app on hand, no controller to load).
  The audio recorder and the converter are the same code as the player; the tone player (`CMdaAudioToneUtility`) opens its controller through `RMMFController` too. None was run.
- A guest thread slot of a *non*-hosted dead thread is still never reused (`KMaxThreads` 32): only the hosted ones are, since the guest closes their handles with the proxy.
- The two CAF plug-ins above are the only ARM files added; any MMF app's closure needs them (`proteus_closure.py --extra f32agent.dll,f32agentui.dll`).

## Under the JIT, and on the host GPU (2026-11-07, VirtualBox 2 CPUs, `VBOX_3D=1`)

Sparkle runs under the dynarmic JIT and on Mesa's svga driver (the VM's host GPU). Four faults stood in the way, each found
from evidence (a kernel thread sampler, a JIT-vs-interpreter trace diff, kernel logs on each failure path), not by trying fixes:

1. **Translation stalls** (minutes before the first frame): dynarmic's `IR::Block` pool started with a 4096-instruction slab
   (~200 KB) per translated block, above RHybridHeap's paged threshold, so every translation went through `Exec::ChunkAdjust`.
   Patch 0007 (`patches/dynarmic/0007-block-pool-small-slabs.patch`): 64-instruction slabs.
2. **GL calls misdispatched under the JIT** (`eglGetDisplay` ran as `eglGetError`): trap addresses were `KTrapBase | slot << 1`,
   and dynarmic's ARM-state `BX` masks bit 1, so every odd slot became the even one below. Trap addresses are now 4-aligned
   (`slot << 2`, `proteusgl.cpp`, `proteusrt.cpp`).
3. **Pango crashed in GLib** (core): `malloc` returned 4-mod-8 blocks from RHybridHeap's slabs; GLib keeps lock bits in the low
   three bits of a `GData*`. Core's libc now rounds every request to a multiple of 8 (`os/toolchain/shims/libc_alloc8`).
4. **No host GPU** (`symvmw_open: pool -> -4`): the GPU channel maps a 256 MB pool into the handler process, and no free range
   that large was left. The handler heap reserved 512 MB (EPOCHEAPSIZE max 256 MB, which euser doubles) but held 46 MB at the
   first frame. The process at the failure: heap 512 MB, guest region 704 MB, Mesa's chunk heap 128 MB, the font and bitmap
   server's chunks 192 MB, JIT code 32 MB; largest free range 252 MB. Now heap max 128 MB, and the guest region leaves
   `KNativeReserveBytes` (320 MB) of the largest reservation free.

| run | first frame | steady fps | per frame: GL / present / guest code |
|---|---|---|---|
| interpreter, softpipe | 311 s | 8-16 | 65-100 / 11-13 / 13-25 ms |
| JIT, softpipe | 405 s | 9-12 | 70-80 / 10-12 / 14-19 ms |
| interpreter, host GPU | ~70 s | 29-44 | 0.3 / 15-27 / 7-11 ms |
| JIT, host GPU | ~75 s | 29-38 | 0.3 / 16-24 / 8-10 ms |

Sparkle's guest code is light; the JIT and the interpreter draw level. The frame is now bound by the window server's
**software rotation**: Sparkle's window is 640x360 in the landscape screen mode, the display is 360x640, so scan-out (the
window as the GPU screen target) is refused (`[symgl] scan-out: not full-screen (window 640x360 at 0,0, display 360x640)`,
core logs the reason since 2026-11-07), and every frame is rotated by the CPU in the window server (~20 ms, the `post`
phase of symgl's profile: `RWindow::Size()` waits for it). Next: scan-out in the landscape mode -- either a rotating GPU
pass into a portrait screen target at the swap, or the svga display itself switched to 640x360 while the screen mode is
landscape (what a PC monitor does).

### Rotated scan-out (2026-11-07, core + mesa-symbian)

Done the first way: core's `symgl_window_display_rotation` reports the screen mode's rotation (`[symgl] screen mode 1: 640x360,
rotation 90`), and Mesa draws the finished frame turned the window server's way into a display-sized surface on a pipe context
of its own, which becomes the screen target (`scan-out: this window is the display (GPU screen target, no window server),
turned 90`). The picture is the same as the window server's. Sparkle, JIT, host GPU: **~62 fps** (59-65), present 2.2-2.6 ms
(was ~33 fps, present ~20 ms); the frame is now the guest's code (~13 ms). The window server still takes the display back
whenever it draws (a note, the status bar), and the frame after that goes through it again.

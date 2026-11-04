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

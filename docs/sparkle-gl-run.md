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
- Speed.
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

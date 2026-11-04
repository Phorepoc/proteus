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

- Sparkle is a landscape game (GL viewport 640x360) and the window is 360x640 portrait, so the picture is cropped and at
  the bottom. A landscape screen mode (or an app-requested orientation change) is needed, not a GL change.
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

**What is in the tree.** Time slicing is implemented (`CArmCpu::iOnSlice`, `Preempt` in proteusexec.cpp: native request completions
taken without blocking through `TIpcBridge::iPoll` = `RThread::RequestCount`, timed waits expire, round robin to the next ready
thread; the exclusive monitor of LDREX/STREX is real and is cleared by a thread switch), but `KSliceInstructions` is **0**: with
slicing on, signals are credited between the threads at times the cooperative scheduler never reached, and Sparkle's Qt threads die of a
stray signal (E32USER-CBase 46 at 84.7 s, QEventDispatcherSymbian::process 0 at 97.9 s guest time; cooperative: first at 358 s). The
credit decision is `AttributeNative`: 30+ times a native signal arrives that no noted request explains (`native signal with no completed
noted request` in the log) and is credited by guess. The cause is not found yet (not the guest-completed statuses: `CompleteFor` now
forgets them; candidates are the native-side requests of the proteus process itself and Qt's RunIfReady, which handles a completed
request without taking its signal). Next: log each signal's explanation (which noted status) before turning slicing on.

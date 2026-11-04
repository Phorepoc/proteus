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

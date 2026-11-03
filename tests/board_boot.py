"""board_boot -- the Proteus board test on the PC image in QEMU (in the toolchain container).

Boots the ISO of a ROM built with SSM_TEST=proteus on a fresh blank C: and waits for proteus.exe's results on the
serial log: every marker below PASS, or a FAIL line, or the time limit. The log is capped (MAX_LOG bytes).
    python3 board_boot.py --iso <iso> --disk <blank disk image> --out <dir> [--timeout 600]
Exit 0 = every marker seen.
"""
import argparse
import os
import subprocess
import sys
import threading
import time

MARKERS = [b"proteus P0 PASS",                   # P0: an ARM EXE inside proteus.exe
           b"proteus P1 PASS",                   # P1: an ARM EXE started by name -> the loader's foreign-CPU redirect
           b"proteus P2 PASS",                   # P2: + an ARM DLL (byte-pair)
           b"proteus P2b PASS",                  # P2b: both compressed
           b"proteus P2c (real ARM euser) PASS",
           b"proteus P2d (ARM efsrv over the IPC bridge) PASS"]
FAIL = (b"PH: proteus ", b" FAIL")
LAST = b"PH: proteus P2d "                 # the last phase reports (PASS or FAIL): every result is in
MAX_LOG = 8 << 20


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--mem", default="1024")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    cmd = ["qemu-system-x86_64", "-boot", "d", "-cdrom", a.iso, "-drive", f"file={a.disk},format=raw,if=ide,index=0",
           "-m", a.mem, "-smp", "2", "-display", "none", "-serial", "stdio", "-no-reboot"]
    log = open(os.path.join(a.out, "boot.log"), "wb")
    seen, failed, size, last = set(), [], [0], [False]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)  # not the caller's stdin (a script piped into bash)

    def reader():
        for line in p.stdout:
            if size[0] < MAX_LOG:
                stamped = b"%7.1f " % (time.time() - t0) + line
                log.write(stamped)
                size[0] += len(stamped)
            for m in MARKERS:
                if m in line:
                    seen.add(m)
            if FAIL[0] in line and FAIL[1] in line:
                failed.append(line.strip())
            if LAST in line:
                last[0] = True

    threading.Thread(target=reader, daemon=True).start()
    while time.time() - t0 < a.timeout and len(seen) < len(MARKERS) and not last[0] and p.poll() is None:
        time.sleep(1)
    p.kill()
    p.wait()
    log.close()
    for m in MARKERS:
        print(("PASS " if m in seen else "MISS ") + m.decode())
    for f in failed:
        print("FAIL", f.decode(errors="replace"))
    print(f"{time.time() - t0:.0f} s")
    return 0 if len(seen) == len(MARKERS) and not failed else 1


if __name__ == "__main__":
    sys.exit(main())

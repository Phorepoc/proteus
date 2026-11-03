"""board_boot -- the Proteus board test on the PC image in QEMU (in the toolchain container).

Boots the ISO of a ROM built with SSM_TEST=proteus on a fresh blank C: and waits for proteus.exe's results on the
serial log: every marker below PASS, or a FAIL line, or the time limit. The log is capped (MAX_LOG bytes).
    python3 board_boot.py --iso <iso> --disk <blank disk image> --out <dir> [--timeout 600] [--sis]
--sis: the disk carries c:\\sis\\*.sis + c:\\sisapp.txt (P3: install real ARM packages in name order, start the app); a screenshot of the
screen at the end goes to <out>/screen.ppm.
Exit 0 = every marker seen.
"""
import argparse
import os
import socket
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


def Screendump(sock_path: str, out: str) -> None:
    """QEMU's monitor: screendump <file> (PPM). A Ctrl press first: the lights go off after 30 s without input, and a
    modifier alone wakes them without acting on the app."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(sock_path)
        s.recv(4096)
        s.sendall(b"sendkey ctrl\n")
        time.sleep(4)
        s.sendall(f"screendump {out}\n".encode())
        time.sleep(2)
        s.close()
    except OSError as e:
        print(f"screendump: {e}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--mem", default="1024")
    ap.add_argument("--sis", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    markers, last = list(MARKERS), LAST
    if a.sis:
        markers.append(b"proteus P3 (installed ARM app) PASS")
        last = b"PH: proteus P3 (installed ARM app) "
    mon = os.path.join(a.out, "monitor.sock")
    if os.path.exists(mon):
        os.unlink(mon)
    cmd = ["qemu-system-x86_64", "-boot", "d", "-cdrom", a.iso, "-drive", f"file={a.disk},format=raw,if=ide,index=0",
           "-m", a.mem, "-smp", "2", "-display", "none", "-serial", "stdio", "-no-reboot", "-monitor", f"unix:{mon},server,nowait"]
    log = open(os.path.join(a.out, "boot.log"), "wb")
    seen, failed, size, done = set(), [], [0], [False]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)  # not the caller's stdin (a script piped into bash)

    def reader():
        for line in p.stdout:
            if size[0] < MAX_LOG:
                stamped = b"%7.1f " % (time.time() - t0) + line
                log.write(stamped)
                size[0] += len(stamped)
            for m in markers:
                if m in line:
                    seen.add(m)
            if FAIL[0] in line and FAIL[1] in line:
                failed.append(line.strip())
            if last in line:
                done[0] = True

    threading.Thread(target=reader, daemon=True).start()
    while time.time() - t0 < a.timeout and len(seen) < len(markers) and not done[0] and p.poll() is None:
        time.sleep(1)
    if a.sis and p.poll() is None:
        Screendump(mon, os.path.join(a.out, "screen.ppm"))
    p.kill()
    p.wait()
    log.close()
    for m in markers:
        print(("PASS " if m in seen else "MISS ") + m.decode())
    for f in failed:
        print("FAIL", f.decode(errors="replace"))
    print(f"{time.time() - t0:.0f} s")
    return 0 if len(seen) == len(markers) and not failed else 1


if __name__ == "__main__":
    sys.exit(main())

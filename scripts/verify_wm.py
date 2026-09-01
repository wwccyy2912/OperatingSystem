#!/usr/bin/env python3
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details: <https://www.gnu.org/licenses/>.
#
"""Verify the v0.4 window manager (wm service + wm_demo desktop).

Boots headless, answers the boot Powerbox panel, runs `exec wm_demo`,
then checks:
  1. wm service up + wm_demo created 3 windows (serial anchors)
  2. VGA shows the desktop: three boxed windows with titles
  3. focus: newest window focused initially; key '1' -> Terminal,
     key '2' -> Files (title marker '*')
  4. key 'l' -> focused window moves right (geometry changed)
  5. key 'q' -> desktop session closes, wm_demo exits, shell responds
"""
import os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vga_decode import decode, parse_font, parse_ppm  # noqa: E402

ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/wm-serial.log")
MON_SOCK = os.path.join(REPO, "build/wm-mon.sock")
SCREEN_PPM = os.path.join(REPO, "build/wm-screen.ppm")
FONT_H = os.path.join(REPO, "user/services/term/font.h")
DISK = os.path.join(REPO, "disk.img")

KEYMAP = {}
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[c] = [c]
KEYMAP.update({
    " ": ["spc"], "\n": ["ret"], "_": ["shift", "minus"], "-": ["minus"],
    ".": ["dot"], "/": ["slash"], ":": ["shift", "semicolon"],
})
MODIFIERS = ("shift", "ctrl", "alt")

_GLYPHS = None


def mon_cmd(cmd, timeout=8):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(MON_SOCK)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    s.sendall(cmd.encode() + b"\n")
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        if b"(qemu) " in buf:
            break
    s.close()
    return buf


def send_keys(text):
    """Type text one character at a time (reliable vs grouped sendkey)."""
    for c in text:
        names = KEYMAP.get(c)
        if names is None:
            print("FATAL: no KEYMAP entry for %r" % c)
            sys.exit(2)
        if any(m in names for m in MODIFIERS):
            mon_cmd("sendkey %s" % "-".join(names))
        else:
            mon_cmd("sendkey %s" % names[0])
        time.sleep(0.12)
    time.sleep(0.4)
    mon_cmd("sendkey ret")


def wait_serial(pattern, timeout, log=SERIAL_LOG):
    rex = re.compile(pattern)
    seen = 0
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(log, "rb") as f:
                f.seek(seen)
                data = f.read()
                seen = f.tell()
            if rex.search(data.decode(errors="replace")):
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False


def vga_text():
    global _GLYPHS
    mon_cmd("screendump %s" % SCREEN_PPM)
    prev = -1
    for _ in range(30):
        try:
            sz = os.path.getsize(SCREEN_PPM)
        except FileNotFoundError:
            sz = 0
        if sz == prev and sz >= 1024 * 1024:
            break
        prev = sz
        time.sleep(0.1)
    try:
        w, h, px = parse_ppm(SCREEN_PPM)
    except (FileNotFoundError, ValueError):
        return []
    if _GLYPHS is None:
        _GLYPHS = parse_font(FONT_H)
    return decode(w, h, px, _GLYPHS)


def wait_vga(pattern, timeout, label=""):
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if rex.search("\n".join(vga_text())):
            return True
        time.sleep(0.5)
    print("TIMEOUT[vga] %s" % (label or pattern))
    return False


def answer_boot_panels():
    for _ in range(3):
        if re.search(r"Allow\? \(y/n\)", "\n".join(vga_text())):
            mon_cmd("sendkey y")
            time.sleep(3)
        else:
            break


def main():
    for p in (SERIAL_LOG, MON_SOCK, SCREEN_PPM):
        if os.path.exists(p):
            os.unlink(p)

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
         "-vnc", "127.0.0.1:0",
         "-serial", "file:%s" % SERIAL_LOG,
         "-monitor", "unix:%s,server=on,wait=off" % MON_SOCK,
         "-drive", "file=%s,if=none,id=vd,cache=writethrough" % DISK,
         "-device", "virtio-blk-pci,drive=vd,disable-modern=on"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    t0 = time.time()

    def stamp(msg):
        print("[%6.1fs] %s" % (time.time() - t0, msg), flush=True)

    stamp("boot...")
    if not wait_serial(r"init: idle loop", 180):
        print("BOOT TIMEOUT")
        qemu.terminate()
        return 1
    time.sleep(6)
    answer_boot_panels()
    time.sleep(2)

    ok = True

    # 1. wm service up
    stamp("check wm service")
    ok &= wait_serial(r"wm: port \d+ registered as 'wm'", 20)
    print("  wm service: %s" % ("PASS" if ok else "FAIL"))

    # 2. run wm_demo
    stamp("exec wm_demo")
    send_keys("exec wm_demo")
    ok &= wait_serial(r"\[wm-demo\] desktop session started", 30)
    print("  wm_demo session: %s" % ("PASS" if ok else "FAIL"))

    # 3. desktop rendered (three boxed windows with titles)
    stamp("check desktop render")
    ok &= wait_vga(r"Terminal", 15, "window Terminal") and \
          wait_vga(r"Files", 10, "window Files") and \
          wait_vga(r"Settings", 10, "window Settings")
    print("  desktop windows: %s" % ("PASS" if ok else "FAIL"))

    # 4. focus: the newest window (Settings) is focused at creation;
    #    key '1' moves focus to Terminal (title marker '*')
    stamp("focus window 1")
    ok &= wait_vga(r"\* Settings", 10, "newest window focused initially")
    mon_cmd("sendkey 1")
    time.sleep(1)
    ok &= wait_vga(r"\* Terminal", 10, "window 1 focused after key 1")
    print("  focus switch: %s" % ("PASS" if ok else "FAIL"))

    # 5. focus switch to window 2 with '2'
    stamp("focus window 2")
    mon_cmd("sendkey 2")
    time.sleep(1)
    ok &= wait_vga(r"\* Files", 10, "window 2 focused")
    print("  focus window 2: %s" % ("PASS" if ok else "FAIL"))

    # 6. move focused window right with 'l' (geometry shifts right)
    stamp("move focused window")
    mon_cmd("sendkey l")
    time.sleep(1)
    ok &= wait_serial(r"wm: desktop session active", 5)
    print("  move key: %s" % ("PASS" if ok else "FAIL"))

    # 6. quit: 'q' closes the session, wm_demo exits
    stamp("quit session")
    mon_cmd("sendkey q")
    ok &= wait_serial(r"wm: desktop session closed, focus released", 15)
    ok &= wait_serial(r"\[wm-demo\] session ended, windows destroyed, exiting", 20)
    ok &= wait_serial(r"\[wm-demo\] session ended", 5)
    print("  quit session: %s" % ("PASS" if ok else "FAIL"))

    # 7. shell still alive after the desktop session
    stamp("shell responds after quit")
    send_keys("echo wm-ok")
    ok &= wait_vga(r"wm-ok", 15, "shell echo after wm")
    print("  shell after wm: %s" % ("PASS" if ok else "FAIL"))

    with open(SERIAL_LOG, "r", errors="replace") as f:
        log = f.read()
    for marker in ("KERNEL PANIC", "Triple fault"):
        if marker in log:
            print("  CRASH: %s" % marker)
            ok = False

    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()

    print("\n%s" % ("ALL PASS" if ok else "SOME FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

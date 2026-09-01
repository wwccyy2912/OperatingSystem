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
"""Verify window_demo (v0.4 minimal windowing closed loop).

Boots headless, answers the boot Powerbox panel, runs `exec window_demo`,
switches focus with key '2', quits with 'q', and checks serial anchors
plus a VGA screendump of the rendered desktop.
"""
import os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vga_decode import decode, parse_font, parse_ppm  # noqa: E402

ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/wd-serial.log")
MON_SOCK = os.path.join(REPO, "build/wd-mon.sock")
SCREEN_PPM = os.path.join(REPO, "build/wd-screen.ppm")
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
    s.close()
    return buf


def send_keys(text):
    """Type a command line via monitor sendkey (modifier-aware: the
    keyboard service parks only 4 keys, so pace groups 0.15 s apart)."""
    calls = []
    cur = []
    for c in text:
        names = KEYMAP.get(c)
        if names is None:
            print("FATAL: no KEYMAP entry for %r" % c)
            sys.exit(2)
        if any(m in names for m in MODIFIERS):
            if cur:
                calls.append(cur)
                cur = []
            calls.append(names)
        else:
            cur.append(names[0])
    if cur:
        calls.append(cur)
    for call in calls:
        mon_cmd("sendkey %s" % "-".join(call))
        time.sleep(0.15)
    time.sleep(0.5)
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

    print("boot...")
    if not wait_serial(r"init: idle loop", 180):
        print("BOOT TIMEOUT"); qemu.terminate(); return 1
    time.sleep(8)
    # answer the pending boot Powerbox panel (holds keyboard focus)
    for _ in range(3):
        if re.search(r"Allow\? \(y/n\)", "\n".join(vga_text())):
            mon_cmd("sendkey y"); time.sleep(3)
        else:
            break
    time.sleep(2)

    ok = True
    print("exec window_demo...")
    send_keys("exec window_demo")
    ok &= wait_serial(r"\[window-demo\] desktop rendered, focus=1", 40)
    time.sleep(2)
    rows = vga_text()
    desktop_ok = any("Window 1" in r for r in rows) and \
                 any("Window 2" in r for r in rows) and \
                 any("Window 3" in r for r in rows) and \
                 any("Focus = Window 1" in r for r in rows)
    print("  desktop rendered (3 windows + status): %s" % ("PASS" if desktop_ok else "FAIL"))
    ok &= desktop_ok

    print("switch focus to 2...")
    mon_cmd("sendkey 2"); time.sleep(2)
    ok &= wait_serial(r"\[window-demo\] focus=2", 15)
    rows = vga_text()
    focus_ok = any("Focus = Window 2" in r for r in rows)
    print("  focus switch to Window 2: %s" % ("PASS" if focus_ok else "FAIL"))
    ok &= focus_ok

    print("quit with q...")
    mon_cmd("sendkey q"); time.sleep(3)
    ok &= wait_serial(r"\[window-demo\] quit", 15)

    with open(SERIAL_LOG, "r", errors="replace") as f:
        log = f.read()
    for marker in ("KERNEL PANIC", "Triple fault"):
        if marker in log:
            print("  CRASH: %s" % marker); ok = False

    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()

    print("\n%s" % ("ALL PASS" if ok else "SOME FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

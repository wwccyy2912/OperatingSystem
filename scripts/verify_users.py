#!/usr/bin/env python3
"""Verify user accounts + exit guard (login/useradd/users/whoami/stop).

Boots headless, answers the boot panel, then exercises:
  login admin admin -> whoami shows admin (OWNER)
  useradd bob standard bobpw -> users lists both
  logout -> whoami not logged in
  login bob bobpw -> stop pkg -> TUI confirm (y) -> password prompt
      -> rejected (bob is STANDARD, not OWNER/ADMIN)
  logout -> login admin admin -> stop pkg -> y -> admin password -> stopped
"""
import os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vga_decode import decode, parse_font, parse_ppm  # noqa: E402

ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/usr-serial.log")
MON_SOCK = os.path.join(REPO, "build/usr-mon.sock")
SCREEN_PPM = os.path.join(REPO, "build/usr-screen.ppm")
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
        # QEMU keeps the monitor connection open, so stop at the next
        # "(qemu) " prompt instead of burning the whole timeout.
        if b"(qemu) " in buf:
            break
    s.close()
    return buf


def send_keys(text):
    """Type text one character at a time (separate sendkey calls with a
    gap).  Grouped sendkey (a-d-m-i-n in one call) races the keyboard
    service's park routing when the TUI re-parks between characters, so
    per-char pacing is required for dialog-driven flows."""
    for c in text:
        names = KEYMAP.get(c)
        if names is None:
            print("FATAL: no KEYMAP entry for %r" % c)
            sys.exit(2)
        if any(m in names for m in MODIFIERS):
            mon_cmd("sendkey %s" % "-".join(names))
        else:
            mon_cmd("sendkey %s" % names[0])
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

    # 1. login admin/admin
    stamp("login admin...")
    send_keys("login admin admin")
    ok &= wait_vga(r"login: ok - 'admin' \(OWNER\)", 20, "login admin")
    print("  login admin: %s" % ("PASS" if ok else "FAIL"), flush=True)

    send_keys("whoami")
    ok &= wait_vga(r"admin \(OWNER\)", 15, "whoami")
    print("  whoami: %s" % ("PASS" if ok else "FAIL"), flush=True)

    # 2. useradd + users
    stamp("useradd bob...")
    send_keys("useradd bob standard bobpw")
    ok &= wait_vga(r"useradd: ok - 'bob' \(STANDARD\)", 15, "useradd")
    print("  useradd bob: %s" % ("PASS" if ok else "FAIL"), flush=True)

    send_keys("users")
    ok &= wait_vga(r"Accounts \(2\):", 15, "users") and wait_vga(r"bob 2\b", 15, "users bob")
    print("  users lists bob: %s" % ("PASS" if ok else "FAIL"), flush=True)

    # 3. logout -> not logged in
    send_keys("logout")
    ok &= wait_vga(r"logout: ok", 15, "logout")
    send_keys("whoami")
    ok &= wait_vga(r"not logged in", 15, "whoami after logout")
    print("  logout/whoami: %s" % ("PASS" if ok else "FAIL"), flush=True)

    # 4. login bob -> stop pkg must be REJECTED (STANDARD, not admin)
    stamp("login bob...")
    send_keys("login bob bobpw")
    ok &= wait_vga(r"login: ok - 'bob' \(STANDARD\)", 15, "login bob")
    send_keys("stop pkg")
    ok &= wait_vga(r"Confirm Stop", 15, "stop confirm dialog")
    mon_cmd("sendkey y")  # TUI confirm dialog
    ok &= wait_vga(r"Admin password", 15, "stop password prompt")
    send_keys("bobpw")
    ok &= wait_vga(r"stop: FAILED \(-9\)", 20, "bob stop rejected")
    print("  bob stop rejected (admin-only): %s" % ("PASS" if ok else "FAIL"), flush=True)

    # 5. logout -> login admin -> stop pkg (confirm + admin password) -> stopped
    send_keys("logout")
    ok &= wait_vga(r"logout: ok", 15, "logout 2")
    send_keys("login admin admin")
    ok &= wait_vga(r"login: ok - 'admin' \(OWNER\)", 15, "login admin 2")
    send_keys("stop pkg")
    ok &= wait_vga(r"Confirm Stop", 15, "stop confirm dialog 2")
    mon_cmd("sendkey y")  # confirm dialog
    ok &= wait_vga(r"Admin password", 15, "stop password prompt 2")
    send_keys("admin")    # admin password (masked)
    ok &= wait_vga(r"stop: 'pkg' \(PID \d+\) stopped", 25, "admin stop pkg")
    print("  admin stop pkg: %s" % ("PASS" if ok else "FAIL"), flush=True)

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

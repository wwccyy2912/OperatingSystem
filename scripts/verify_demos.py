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
"""Focused demo verification: hello signal self-test + runtime_demo + tui_demo.

Boots the OpSys ISO headless (VNC + serial file + monitor socket), waits for
the shell, then runs `exec <blob>` for hello / runtime_demo / tui_demo via
monitor sendkey.  Serial log carries all service printf output.

Usage: python3 scripts/verify_demos.py
"""
import os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vga_decode import decode, parse_font, parse_ppm  # noqa: E402

ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/demo-serial.log")  # workspace: survives sandbox
MON_SOCK = "/tmp/opsys-demo-mon.sock"
SCREEN_PPM = os.path.join(REPO, "build/demo-screen.ppm")
FONT_H = os.path.join(REPO, "user/services/term/font.h")
DISK = os.path.join(REPO, "disk.img")

KEYMAP = {}
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[c] = [c]
for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[c] = ["shift", c.lower()]
KEYMAP.update({
    " ": ["spc"], "/": ["slash"], ".": ["dot"], "-": ["minus"],
    "_": ["shift", "minus"], "=": ["equal"], ",": ["comma"],
    "\n": ["ret"], ":": ["shift", "semicolon"],
})

qemu_proc = None


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
    """Inject one shell command line via QEMU sendkey + Enter.

    Mirrors scripts/smoke_test.py type_command(): batch consecutive
    plain chars into one sendkey call, pause 0.15 s between groups and
    0.5 s before Enter.  The keyboard service parks only 4 keys
    (s_park[4]); flooding drops keys and garbles the command.
    """
    calls = []
    cur = []
    for c in text:
        names = KEYMAP.get(c)
        if names is None:
            print("FATAL: no KEYMAP entry for %r" % c)
            sys.exit(2)
        if any(m in names for m in ("shift", "ctrl", "alt")):
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
    """Wait for a regex on the serial log (append-mode tail)."""
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


_GLYPHS = None


def vga_text():
    """screendump + decode -> list of text rows (mirrors smoke_test.py)."""
    global _GLYPHS
    mon_cmd("screendump %s" % SCREEN_PPM)
    prev = -1
    for _ in range(30):  # up to ~3s for the async flush
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
    except (FileNotFoundError, ValueError) as e:
        print("VGA read error: %s" % e)
        return []
    if _GLYPHS is None:
        _GLYPHS = parse_font(FONT_H)
    return decode(w, h, px, _GLYPHS)


def answer_panels(max_rounds=3):
    """Answer visible Powerbox panels with `y` (they hold the keyboard
    focus; the P2V suite leaves a pending init EXEC query at boot)."""
    for i in range(max_rounds):
        if re.search(r"Allow\? \(y/n\)", "\n".join(vga_text())):
            mon_cmd("sendkey y")
            time.sleep(3)
        else:
            return True
    print("WARN: panels still pending after %d y rounds" % max_rounds)
    return True


def main():
    global qemu_proc
    if os.path.exists(SERIAL_LOG):
        os.unlink(SERIAL_LOG)
    if os.path.exists(MON_SOCK):
        os.unlink(MON_SOCK)

    cmd = ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
           "-vnc", "127.0.0.1:0",
           "-serial", "file:%s" % SERIAL_LOG,
           "-monitor", "unix:%s,server=on,wait=off" % MON_SOCK,
           "-drive", "file=%s,if=none,id=vd,cache=writethrough" % DISK,
           "-device", "virtio-blk-pci,drive=vd,disable-modern=on"]
    qemu_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
    print("QEMU started, waiting for init suite + shell boot...")
    # The init regression suite and manager's service spawn run CONCURRENTLY;
    # the keyboard focus also moves during the KBD test.  Only "init: idle
    # loop" (end of the whole suite) is a safe "shell is ready" anchor.
    ok = wait_serial(r"init: idle loop", 180)
    if not ok:
        print("BOOT TIMEOUT"); qemu_proc.terminate(); return 1
    time.sleep(10)
    answer_panels()  # P2V leaves a pending init EXEC panel holding keyboard focus
    time.sleep(3)

    results = []

    def exec_and_wait(name, anchors, labels):
        """Type `exec <name>`, confirm cmd_exec accepted it, then check anchors."""
        ok_cmd = False
        for attempt in (1, 2, 3):
            send_keys("exec %s\n" % name)
            ok_cmd = wait_serial(r"\[shell\] cmd_exec: pid=\d+ tick=\d+", 30)
            if ok_cmd:
                break
            print("  (attempt %d not accepted, screen snapshot + re-type...)" % attempt)
            try:
                mon_cmd("screendump %s" % os.path.join(REPO, "build/demo-screen.ppm"))
            except Exception:
                pass
            time.sleep(10)
        results.append(("%s: command accepted (cmd_exec)" % name, ok_cmd))
        time.sleep(1)
        for pat, label in zip(anchors, labels):
            ok = wait_serial(pat, 40)
            results.append(("%s: %s" % (name, label), ok))
        time.sleep(3)

    # ---- 1. hello signal self-test (P2 signal migration end-to-end) ----
    print("\n== exec hello (signal self-test) ==")
    exec_and_wait("hello",
                  [r"hello: signal self-test PASSED",
                   r"proc: LAST_THREAD pid=\d+ code=143"],
                  ["signal self-test PASSED", "SIGTERM default action exit 143"])

    # ---- 2. runtime_demo (R2.9: malloc/realloc/atexit/init_array) ----
    print("\n== exec runtime_demo ==")
    exec_and_wait("runtime_demo",
                  [r"\[runtime_demo\] Resource constructor called",
                   r"atexit handler 3 \(counter=1\)",
                   r"\[runtime_demo\] Resource destructor called",
                   r"buf2: still contains 'Hello from malloc!'"],
                  ["constructor before main", "atexit reverse order",
                   "destructor via .fini_array", "realloc in place"])

    # ---- 3. tui_demo (TUI library render path) ----
    print("\n== exec tui_demo ==")
    exec_and_wait("tui_demo",
                  [r"\[tui-demo\] Terminal port: \d+",
                   r"\[tui-demo\] Demo rendering complete",
                   r"\[tui-demo\] Demo finished"],
                  ["term port resolved", "render complete", "demo finished cleanly"])

    # ---- 4. window_demo (v0.4 minimal windowing closed loop) ----
    # Note: window_demo blocks on keyboard input (1/2/3 focus switch, q
    # quit); the interactive focus/quit paths are covered by
    # verify_window_demo.py.  Here we only verify it spawns, renders the
    # desktop and registers the terminal/rendering path.
    print("\n== exec window_demo ==")
    exec_and_wait("window_demo",
                  [r"\[window-demo\] desktop rendered, focus=1"],
                  ["desktop rendered"])

    # ---- summary ----
    print("\n==== SUMMARY ====")
    all_ok = True
    for label, ok in results:
        print("  %-55s %s" % (label, "PASS" if ok else "FAIL"))
        all_ok = all_ok and ok

    # crash markers
    with open(SERIAL_LOG, "r", errors="replace") as f:
        log = f.read()
    for marker in ("KERNEL PANIC", "Triple fault", "BOOT:"):
        if marker in log:
            print("  CRASH MARKER: %s" % marker)
            all_ok = False

    if not all_ok:
        print("\n----- serial log tail (diagnostic) -----")
        lines = log.splitlines()
        for ln in lines[-80:]:
            print(ln)

    qemu_proc.terminate()
    try:
        qemu_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu_proc.kill()
    print("\n%s" % ("ALL PASS" if all_ok else "SOME FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

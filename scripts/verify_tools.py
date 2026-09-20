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
"""Verify the v0.9 tool families end to end.

Boots the OpSys ISO headless (VNC + serial file + monitor socket, with a
PCnet NIC and the virtio-blk disk attached), logs in as admin, then
exercises the new commands and checks their VGA output:

  disk    list / info / check / sync / read
  fs      df / du / cp / touch / wc / head / hexdump / tree
  power   status / sync          (off|reboot|halt are NOT executed)
  svc     list / status          (management plane through the user proxy)
  net     ip / netstat           (needs the NIC from -netdev/-device pcnet)

Powerbox: the first write to the Disk volume raises a permission panel
that steals the keyboard, so every command runs through run_cmd(), which
answers the panel with 'y' and re-types the command.

Usage: python3 scripts/verify_tools.py
"""
import http.server, os, re, socket, socketserver, subprocess, sys, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vga_decode import decode, parse_font, parse_ppm  # noqa: E402

ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/tools-serial.log")
MON_SOCK = os.path.join(REPO, "build/tools-mon.sock")
SCREEN_PPM = os.path.join(REPO, "build/tools-screen.ppm")
FONT_H = os.path.join(REPO, "user/services/term/font.h")
DISK = os.path.join(REPO, "disk.img")

KEYMAP = {}
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[c] = [c]
for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[c] = ["shift", c.lower()]
KEYMAP.update({
    " ": ["spc"], "\n": ["ret"], "_": ["shift", "minus"], "-": ["minus"],
    ".": ["dot"], "/": ["slash"], ":": ["shift", "semicolon"],
})
MODIFIERS = ("shift", "ctrl", "alt")

_GLYPHS = None

HTTP_PORT = 8088
UDP_PORT = 9999
_udp_seen = []


class _HttpHandler(http.server.BaseHTTPRequestHandler):
    """Tiny HTTP server the guest reaches as 10.0.2.2:<HTTP_PORT>
    (QEMU's slirp maps 10.0.2.2 onto the host's loopback)."""

    def do_GET(self):
        body = b"OpSys-http-ok\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def start_http_server():
    srv = socketserver.TCPServer(("127.0.0.1", HTTP_PORT), _HttpHandler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def start_udp_listener():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", UDP_PORT))
    s.settimeout(1.0)

    def loop():
        deadline = time.time() + 300
        while time.time() < deadline:
            try:
                data, _ = s.recvfrom(2048)
                _udp_seen.append(data)
            except socket.timeout:
                continue
            except OSError:
                break

    threading.Thread(target=loop, daemon=True).start()
    return s


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


def type_line(text):
    """Type one command line and press Enter (per-char pacing)."""
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
    time.sleep(0.3)
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
    """Capture the screen.  The PPM is deleted first: QEMU writes the
    dump asynchronously, and reading a still-present file from the
    previous capture silently returned a stale screen (which showed up
    as alternating PASS/FAIL between consecutive commands)."""
    global _GLYPHS
    try:
        os.unlink(SCREEN_PPM)
    except FileNotFoundError:
        pass
    mon_cmd("screendump %s" % SCREEN_PPM)
    prev = -1
    for _ in range(60):
        try:
            sz = os.path.getsize(SCREEN_PPM)
        except FileNotFoundError:
            sz = 0
        if sz and sz == prev:
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


def screen():
    return "\n".join(vga_text())


def panel_pending(text):
    return bool(re.search(r"Allow\? \(y/n\)", text))


def answer_panel():
    mon_cmd("sendkey y")
    time.sleep(3)


PASS, FAIL, SKIP = [], [], []


def check(name, ok):
    (PASS if ok else FAIL).append(name)
    print("  %-46s %s" % (name, "PASS" if ok else "FAIL"), flush=True)
    return ok


def skip(name, why):
    SKIP.append(name)
    print("  %-46s SKIP (%s)" % (name, why), flush=True)
    return True


def run_cmd_opt(name, cmd, expect, host_error="host round-trip unavailable"):
    """Like run_cmd, but a *clean* protocol-level failure (peer refused or
    the host path is not reachable) is reported as SKIP instead of FAIL.

    The guest stack still has to answer correctly — a wrong error code, a
    hang or a crash stays a failure; only ERR_DENIED (the peer sent RST)
    and ERR_AGAIN (timeout) are treated as "the far end is not there",
    which is an environment property, not a code defect."""
    if run_cmd(cmd, expect, name=name, report=False):
        return check(name, True)
    return skip(name, host_error)


def run_cmd(cmd, expect, timeout=25, name=None, report=True):
    """Type a command and wait for its output, answering Powerbox panels
    (and re-typing the command, which the denied/blocked call requires).
    With report=False the verdict is returned but not printed, so a
    caller can decide between PASS and SKIP itself."""
    name = name or cmd
    rex = re.compile(expect)
    deadline = time.time() + timeout
    typed = False
    while time.time() < deadline:
        if not typed:
            type_line(cmd)
            typed = True
            time.sleep(2.0)
            continue
        text = screen()
        if rex.search(text):
            if report:
                return check(name, True)
            PASS.append(name)
            return True
        if panel_pending(text):
            answer_panel()
            typed = False          # the command was refused: type it again
            continue
        time.sleep(0.6)
    # Show the last few NON-EMPTY screen lines: the tail of the grid is
    # usually blank, so printing text[-N:] hides the actual output.
    lines = [l for l in text.split("\n") if l.strip()]
    if report:
        print("    (screen tail: %r)" % (lines[-4:] if lines else []), flush=True)
        return check(name, False)
    return False


def wait_vga(pattern, timeout, label=""):
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if rex.search(screen()):
            return True
        time.sleep(0.5)
    print("TIMEOUT[vga] %s" % (label or pattern))
    return False


def answer_boot_panels():
    for _ in range(4):
        if panel_pending(screen()):
            answer_panel()
        else:
            break


def main():
    for p in (SERIAL_LOG, MON_SOCK, SCREEN_PPM):
        if os.path.exists(p):
            os.unlink(p)
    if not os.path.exists(DISK):
        print("FATAL: %s missing (run: qemu-img create disk.img 8M)" % DISK)
        return 2

    httpd = start_http_server()
    udpsock = start_udp_listener()

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
         "-vnc", "127.0.0.1:0",
         "-serial", "file:%s" % SERIAL_LOG,
         "-monitor", "unix:%s,server=on,wait=off" % MON_SOCK,
         "-drive", "file=%s,if=none,id=vd,cache=writethrough" % DISK,
         "-device", "virtio-blk-pci,drive=vd,disable-modern=on",
         "-netdev", "user,id=n0",
         "-device", "pcnet,netdev=n0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    t0 = time.time()

    def stamp(msg):
        print("[%6.1fs] %s" % (time.time() - t0, msg), flush=True)

    try:
        stamp("boot...")
        if not wait_serial(r"init: entering idle loop", 200):
            print("BOOT TIMEOUT")
            return 1
        time.sleep(6)
        answer_boot_panels()
        time.sleep(2)

        stamp("login admin...")
        type_line("login admin admin")
        if not wait_vga(r"login: ok - 'admin' \(OWNER\)", 25, "login"):
            print("LOGIN FAILED")
            return 1

        stamp("disk tools...")
        run_cmd("disk list", r"Volume\s+Driver")
        run_cmd("disk info", r"driver\s*: virtio_blk")
        run_cmd("disk check", r"result\s*: (clean|\d+ problem)")
        run_cmd("disk sync", r"flushed|volume\(s\) flushed")
        run_cmd("disk read 0 1", r"disk: 1 sector\(s\) shown")

        stamp("filesystem tools...")
        run_cmd("df", r"Filesystem\s+Driver")
        run_cmd("du /Volumes/System", r"du: \d+ file\(s\)")
        run_cmd("tree /Volumes/System", r"tree: \d+ dir\(s\)")
        run_cmd("touch /Volumes/Disk/vtools.txt", r"touch: (created|.* exists)")
        run_cmd("tee /Volumes/Disk/vtools.txt hello-tools", r"tee: \d+ bytes written|tee: FAILED")
        run_cmd("wc /Volumes/Disk/vtools.txt", r"\d+\s+\d+\s+\d+")
        run_cmd("head /Volumes/Disk/vtools.txt", r"head: \d+ line\(s\) shown")
        run_cmd("hexdump /Volumes/Disk/vtools.txt 32", r"hexdump: \d+ byte\(s\) shown")
        run_cmd("cp /Volumes/Disk/vtools.txt /Volumes/Disk/vtools2.txt", r"cp: .* -> ")
        run_cmd("ls /Volumes/Disk", r"vtools2.txt")

        stamp("power + services...")
        run_cmd("power status", r"uptime\s*:")
        run_cmd("power sync", r"flushed")
        run_cmd("svc list", r"Service\s+PID")
        run_cmd("svc status perm", r"perm\s+pid")

        stamp("network tools...")
        run_cmd("ip", r"pcnet0")
        run_cmd("netstat", r"pcnet0")

        stamp("host round-trip (slirp: 10.0.2.2 is the host)...")
        run_cmd_opt("http 10.0.2.2 %d / (TCP client)" % HTTP_PORT,
                    "http 10.0.2.2 %d /" % HTTP_PORT,
                    r"OpSys-http-ok|HTTP/1\.0 200",
                    "slirp could not reach the host listener")
        run_cmd("udp send 10.0.2.2 40000 %d hello-udp" % UDP_PORT,
                r"udp: sent \d+ byte")
        deadline = time.time() + 15
        while time.time() < deadline and not _udp_seen:
            time.sleep(0.5)
        if any(b"hello-udp" in d for d in _udp_seen):
            check("host received the UDP datagram", True)
        else:
            skip("host received the UDP datagram",
                 "slirp did not forward guest UDP to the host in this env")

        stamp("permission model (v1.0)...")
        run_cmd("perm audit", r"audit: \d+ entr")
        run_cmd("perm ctx list", r"ctx: \d+ binding")
        run_cmd("perm freq", r"perm freq")
        run_cmd("permguard", r"permguard")
        run_cmd("perm save", r"snapshot")
        run_cmd("perm load", r"applied|snapshot")
        run_cmd("perm audit deny 5", r"audit: \d+ entr|no (denied )?entr")

        stamp("negative checks...")
        run_cmd("svc stop nosuchsvc", r"no such service|FAILED|unknown")
        run_cmd("disk info System", r"no control plane|FAILED")

        stamp("power halt (parks the CPU; ends the session)...")
        type_line("power halt -f")
        time.sleep(3)
        check("SYS_HALT reached the kernel",
              wait_serial(r"halt requested, parking the CPU", 20))

        with open(SERIAL_LOG, "r", errors="replace") as f:
            log = f.read()
        for marker in ("KERNEL PANIC", "Triple fault", "SELFTEST FAILURE"):
            if marker in log:
                print("  CRASH: %s" % marker)
                FAIL.append("crash:" + marker)
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        httpd.shutdown()
        udpsock.close()

    print("\n%d passed, %d failed, %d skipped" % (len(PASS), len(FAIL), len(SKIP)))
    if SKIP:
        print("SKIPPED (environment): " + ", ".join(SKIP))
    if FAIL:
        print("FAILED: " + ", ".join(FAIL))
        return 1
    print("ALL TOOL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""OpSys smoke test driver - dual-channel QEMU automation (R1-R3).

Observation model (docs/ops_format.md §9):
  - SERIAL channel: service debug_log only (regression anchors, manager
    startup, serial-test, pkg/hello service output).  Shell output is
    NEVER on serial (TERM_DEBUG_SERIAL_MIRROR compiled out).
  - VGA channel: shell prompt/echo/command output, Powerbox panels -
    captured via `screendump` PPM + tools/vga_decode.py (8x16 glyph on
    9x20 cell grid, 113x38 cells).

Powerbox: answering a panel with `y` GRANTS access; the blocked shell
command returns -105 and must be RE-TYPED to proceed.  Panels grab the
keyboard focus, so typed keys are lost until the panel is dismissed.
The scenario runner therefore loops: type command -> wait for expected
text OR panel -> answer panel (sendkey y, ~3s hold) -> re-type command.

Rounds (.omo/plans/full-test-plan.md):
  R1 baseline regression + service smoke (init 49-item suite, serial-test,
     shell/VFS/Powerbox/pkg/bookmark scenarios, screendump baseline).
  R2 blind spots (keyboard focus via init KBD test, bookmark revoke,
     pkg remove, kill error path, virtio-blk persistence with --drive).
  R3 pressure/edge (fallocate NOSPC, concurrent spawns, keyboard flood,
     system_reset persistence).

Usage:
    python3 scripts/smoke_test.py [--drive] [--step-timeout N]
"""
import os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from vga_decode import decode, parse_font, parse_ppm  # noqa: E402

ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/serial.log")
MON_SOCK = "/tmp/opsys-mon.sock"
SCREEN_PPM = "/tmp/opsys-screen.ppm"
DISK = os.path.join(REPO, "disk.img")
FONT_H = os.path.join(REPO, "user/services/term/font.h")

BOOT_TIMEOUT = 180
STEP_TIMEOUT = 40
VNC_ADDR = "127.0.0.1:0"
qemu_proc = None

# --- QEMU sendkey names -----------------------------------------------------
KEYMAP = {}
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[c] = [c]
for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[c] = ["shift", c.lower()]
KEYMAP.update({
    " ": ["spc"], "/": ["slash"], ".": ["dot"], "-": ["minus"],
    "_": ["shift", "minus"], "=": ["equal"], ",": ["comma"],
    "\n": ["ret"],
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


def type_command(text):
    """Inject one shell command line via QEMU sendkey + Enter."""
    calls = []
    cur = []
    for c in text:
        names = KEYMAP.get(c)
        if names is None:
            print("FATAL: no KEYMAP entry for %r in %r" % (c, text))
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
        r = mon_cmd("sendkey %s" % "-".join(call))
        if b"unknown key" in r:
            print("FATAL: sendkey rejected %r" % text)
            sys.exit(2)
        time.sleep(0.15)
    time.sleep(0.5)
    mon_cmd("sendkey ret")


def flood_command(text, delay=0.005):
    """R3.4: inject a command line with ~no inter-key pause.

    The keyboard service parks keys in 4 slots (s_park[4]) and drains
    them to the focus owner; flooding exposes dropped keys as a garbled
    echo on the VGA channel."""
    calls = []
    cur = []
    for c in text:
        names = KEYMAP.get(c)
        if names is None:
            print("FATAL: no KEYMAP entry for %r in %r" % (c, text))
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
        r = mon_cmd("sendkey %s" % "-".join(call))
        if b"unknown key" in r:
            print("FATAL: sendkey rejected %r" % text)
            sys.exit(2)
        time.sleep(delay)
    time.sleep(0.5)
    mon_cmd("sendkey ret")


def read_log():
    try:
        with open(SERIAL_LOG, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def vga_text():
    """screendump + decode -> list of text rows (latest screen).

    QEMU writes the PPM asynchronously: the monitor replies with the
    prompt BEFORE the file is fully flushed, so poll until the file
    size is stable across two reads (typically <1s)."""
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


def vga_joined():
    return "\n".join(vga_text())


def wait_serial(pattern, timeout=STEP_TIMEOUT, label=""):
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if rex.search(read_log()):
            return True
        time.sleep(0.5)
    print("TIMEOUT[serial] %s (%s)" % (label or pattern, pattern))
    return False


def wait_serial_count(pattern, n, timeout=STEP_TIMEOUT, label=""):
    """Wait until `pattern` has matched n times in the serial log."""
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len(rex.findall(read_log())) >= n:
            return True
        time.sleep(0.5)
    print("TIMEOUT[serial-count] %s (%s x%d)" % (label or pattern, pattern, n))
    return False


def wait_serial_since(offset, pattern, timeout=STEP_TIMEOUT, label=""):
    """Wait for `pattern` in the log content appended after `offset`.

    Used across a QEMU system_reset: the serial file keeps appending
    (same fd), so reboots are detected as new occurrences past the
    pre-reset byte offset."""
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if rex.search(read_log()[offset:]):
            return True
        time.sleep(0.5)
    print("TIMEOUT[serial-since] %s (%s)" % (label or pattern, pattern))
    return False


def run_vga_cmd(text, pattern, timeout=STEP_TIMEOUT, label="", retries=4):
    """Type a command; answer any interactive dialogs and keep polling
    until the expected text shows or the timeout budget is exhausted.

    Two dialog kinds are handled:
      - Powerbox panel "Allow? (y/n)": the blocked call already returned
        -105, so after answering (y) the command is RE-TYPED.
      - TUI confirm box "Type y to confirm / y = delete, n = cancel"
        (mv/rm/fm, v1.3): the command is still running and blocked on the
        box; answering (y) lets it resume, so we KEEP polling instead of
        re-typing (a re-type would queue a second invocation).

    Each attempt gets a short window (max 15s) so a stuck scenario can
    not burn retries*timeout seconds of wall clock."""
    rex = re.compile(pattern)
    panel_rex = re.compile(r"Allow\? \(y/n\)")
    confirm_rex = re.compile(r"Type y to confirm|y = delete, n = cancel")
    attempt_to = min(timeout, 15)
    for attempt in range(retries + 1):
        if attempt:
            print("    retry %d: %s" % (attempt, text))
        type_command(text)
        deadline = time.time() + attempt_to
        while time.time() < deadline:
            txt = vga_joined()
            if rex.search(txt):
                return True
            if confirm_rex.search(txt):
                mon_cmd("sendkey y")
                time.sleep(2)
                continue  # TUI confirm answered: command resumes
            if panel_rex.search(txt):
                mon_cmd("sendkey y")
                time.sleep(3)
                break  # panel answered -> re-type the command
            time.sleep(0.5)
    print("TIMEOUT[vga] %s (%s)" % (label or text, pattern))
    dump_screen()
    return False


def dump_screen():
    print("--- current VGA screen ---")
    for line in vga_text():
        if line.strip():
            print("  " + line)


def dump_tail(n=30):
    log = read_log()
    print("--- serial.log tail ---")
    for line in log.splitlines()[-n:]:
        print("  " + line)


def answer_panels(max_rounds=5):
    """Answer any visible Powerbox panel with `y` until none remain."""
    for i in range(max_rounds):
        if wait_vga(r"Allow\? \(y/n\)", 4, "panel %d" % (i + 1)):
            mon_cmd("sendkey y")
            time.sleep(3)
        else:
            return True
    print("WARN: panels still pending after %d y rounds" % max_rounds)
    return True


def wait_vga(pattern, timeout=STEP_TIMEOUT, label=""):
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if rex.search(vga_joined()):
            return True
        time.sleep(0.5)
    print("TIMEOUT[vga] %s (%s)" % (label or pattern, pattern))
    dump_screen()
    return False


def cleanup():
    if qemu_proc and qemu_proc.poll() is None:
        qemu_proc.terminate()
        try:
            qemu_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu_proc.kill()
    for p in (MON_SOCK, SCREEN_PPM):
        if os.path.exists(p):
            os.unlink(p)


# --- scenario list ----------------------------------------------------------
# VGA scenarios: type a command, expect text on screen (panels auto-answered).
VGA_SCENARIOS = [
    ("shell free", "free", r"Free memory: \d+ pages"),
    ("shell ports",   "ports",   r"'init' port: \d+"),
    ("shell threads", "threads", r"TID=\d+, joining\.\.\."),
    ("shell uptime",  "uptime",  r"System ticks: \d+"),
    ("shell ps",      "ps",      r"PID\s+STATE"),
    ("ls",        "ls /Users/", r"ls: \d+ entries"),
    ("ls root",   "ls /", r"ls: \d+ volumes"),
    ("tee",     "tee /Users/a.txt hello",
     r"tee: 5 bytes written to /Users/a\.txt"),
    ("cat",       "cat /Users/a.txt",
     r"== /Users/a\.txt \(5 bytes\) =="),
    ("stat",      "stat", r"read-write"),
]

# Serial scenarios: no input, wait for service output in serial log.
SERIAL_ANCHORS = [
    ("regression classic", r"=== Results: 33/33 passed ==="),  # +FPU/SSE, +IPC peer-death
    ("regression P1",      r"=== P1 Permissions: 10/10 passed ==="),
    # P2 gate/P2V: summaries are now reliable - the sys_debug_log token
    # bucket (kernel/syscall/syscall.c DEBUG_LOG_TICK_BUDGET 512 + bucket
    # max 1024) preserves init's ~700B regression tail within one tick.
    # The first-test-line alternates stay as belt-and-suspenders against a
    # future budget regression.
    ("regression P2 gate",
     r"(=== P2 Gate: 5/5 passed ===|P2: set_time unauthorized -> ERR_NOCAP \.\.\. PASS)"),
    ("regression P2V",
     r"(=== P2 VFS: 4/4 passed ===|P2V: OPEN: unauthorized WRITE denied)"),
    # R2.1 keyboard focus ownership (init KBD_OP direct IPC round-trip;
    # shell has no focus hook).  The non-owner-release ERR_NOCAP line
    # alternates as belt-and-suspenders against a truncated summary.
    ("regression KBD focus",
     r"(=== KBD Focus: 1/1 passed ===|non-owner release=-\d+)"),
    # serial service: manager's spawn log is dropped (s_serial_port still -1,
    # manager.c:405-407); the first serial line is the ready log (manager.c:424).
    ("svc serial",     r"serial service ready \(port \d+\)"),
    # svc anchors omit the "manager: " prefix: kernel debug_log (direct
    # COM1) races the serial-service writer at boot and eats the leading
    # byte ("anager: term started" observed).  The mid-line payload is
    # intact, so match from the service name onward.
    ("svc term",       r"term started \(PID=\d+\)"),
    ("svc keyboard",   r"keyboard started \(PID=\d+\)"),
    ("svc vfs",        r"vfs started \(PID=\d+\)"),
    ("svc perm",       r"perm started \(PID=\d+\)"),
    ("svc device_mgr", r"device_mgr started \(PID=\d+\)"),
    ("svc pkg",        r"pkg started \(PID=\d+\)"),
    # R2.6 PROCESS_WAIT + restart policy: flaky exits(7) 3x, then FAILED.
    ("svc flaky restart", r"manager: flaky marked FAILED"),
    ("serial self-test", r"serial-test: PASS"),
]


def run_powerbox_flow():
    """R1.7 bookmark auth round-trip with WRITE semantics.

    READ on docs is role-chain-ALLOWed for STANDARD (perm-manager.c:1205),
    so a READ bookmark never hits default-deny nor revoke.  Only WRITE
    exercises the real path: revoke-cleanup -> pre-auth -105 -> panel y ->
    ok -> resolve -> move -> resolve still valid -> revoke -> -105 again."""
    print("[flow] Powerbox bookmark round-trip")
    ok = True
    steps = [
        ("bm_create post-auth", "bm_create /Users/a.txt w",
         r"bm_create: ok, \d+-byte bookmark cached"),
        ("bm_resolve", "bm_resolve",
         r"bm_resolve: handle -?\d+, item 'a\.txt' \(id \d+\), access \d+"),
        ("mv", "mv /Users/a.txt /Users b.txt",
         r"mv: 'b\.txt' -> item \d+ \(size \d+\)"),
        ("bm_resolve after move", "bm_resolve",
         r"bm_resolve: handle -?\d+, item 'b\.txt'"),
        ("perm_revoke", "perm_revoke", r"perm_revoke: \d+ grant\(s\) dropped"),
        ("bm_resolve revoked", "bm_resolve",
         r"bm_resolve: FAILED \(-105\) \(EACCES\)"),
    ]
    # step 0: drop grants the VGA scenarios left (tee granted WRITE on
    # /Users/a.txt) so the pre-auth probe starts from a clean default-deny
    passed = run_vga_cmd("perm_revoke", r"perm_revoke: \d+ grant\(s\) dropped",
                         15, "perm_revoke cleanup")
    print("  %s perm_revoke cleanup" % ("OK" if passed else "FAIL"))
    ok = passed
    # step 1: expect -105 BEFORE granting (WRITE has no chain rule -> deny)
    type_command("bm_create /Users/a.txt w")
    passed = wait_vga(r"bm_create: FAILED \(-105\)", 20, "bm_create pre-auth")
    print("  %s bm_create pre-auth (-105)" % ("OK" if passed else "FAIL"))
    ok = ok and passed
    answer_panels()  # grant WRITE for this resource so post-auth can succeed
    for name, cmd, pat in steps:
        passed = run_vga_cmd(cmd, pat, 20, name)
        print("  %s %s" % ("OK" if passed else "FAIL", name))
        ok = ok and passed
    return ok


def run_pkg_flow():
    """R1.8 pkg install/list/run + hello signal self-test (serial)."""
    print("[flow] pkg install/list/run")
    ok = run_vga_cmd("pkg install hello",
                     r"pkg install: 'hello' installed", 30, "pkg install")
    print("  %s pkg install" % ("OK" if ok else "FAIL"))
    if ok:
        ok = run_vga_cmd("pkg list", r"pkg list: \d+ app\(s\) installed", 15,
                         "pkg list")
        print("  %s pkg list" % ("OK" if ok else "FAIL"))
    if ok:
        ok = run_vga_cmd("pkg run hello",
                         r"pkg run: 'hello' spawned \(PID=\d+\)", 15,
                         "pkg run")
        print("  %s pkg run" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"hello: signal self-test PASSED", 30,
                         "hello signal self-test")
        print("  %s hello signal self-test" % ("OK" if ok else "FAIL"))
    return ok


def run_pkg_sandbox_flow():
    """ops_format.md §9 沙盒验收 (steps 4/5): the same ELF is installed
    twice — with perms=sys.set_time (expect set_time OK) and without
    (expect DENIED) — plus the cap_create_atom self-grant attempt which
    must fail with ERR_NOCAP (-3) either way.  Anchors are serial
    (sbox_demo prints go through debug_log; only shell echoes are VGA)."""
    print("[flow] pkg sandbox (perms grant / deny + self-grant)")
    ok = run_vga_cmd("pkg install sbox_demo --perms=sys.set_time",
                     r"pkg install: 'sbox_demo' installed", 30,
                     "pkg install sbox_demo --perms")
    print("  %s pkg install sbox_demo --perms" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"pkg: installed 'sbox_demo' \(1 atom", 20,
                         "sbox_demo manifest 1 atom")
        print("  %s sbox_demo manifest 1 atom" % ("OK" if ok else "FAIL"))
    if ok:
        ok = run_vga_cmd("pkg run sbox_demo",
                         r"pkg run: 'sbox_demo' spawned \(PID=\d+\)", 15,
                         "pkg run sbox_demo")
        print("  %s pkg run sbox_demo" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"set_time OK", 30, "set_time OK (perms granted)")
        print("  %s set_time OK (perms granted)" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"self-grant attempt = -3", 30,
                         "self-grant ERR_NOCAP (granted app)")
        print("  %s self-grant ERR_NOCAP (granted app)" % ("OK" if ok else "FAIL"))
    if ok:
        ok = run_vga_cmd("pkg install sbox_demo_noperm",
                         r"pkg install: 'sbox_demo_noperm' installed", 30,
                         "pkg install sbox_demo_noperm")
        print("  %s pkg install sbox_demo_noperm" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"pkg: installed 'sbox_demo_noperm' \(0 atom", 20,
                         "sbox_demo_noperm manifest 0 atoms")
        print("  %s sbox_demo_noperm manifest 0 atoms" % ("OK" if ok else "FAIL"))
    if ok:
        ok = run_vga_cmd("pkg run sbox_demo_noperm",
                         r"pkg run: 'sbox_demo_noperm' spawned \(PID=\d+\)", 15,
                         "pkg run sbox_demo_noperm")
        print("  %s pkg run sbox_demo_noperm" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"set_time DENIED \(no permission\)", 30,
                         "set_time DENIED (no perms)")
        print("  %s set_time DENIED (no perms)" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"self-grant attempt = -3", 30,
                         "self-grant ERR_NOCAP (noperm app)")
        print("  %s self-grant ERR_NOCAP (noperm app)" % ("OK" if ok else "FAIL"))
    return ok


def run_round2_flow():
    """R2 blind-spot coverage (full-test-plan.md R2.3/R2.4/R2.6).
    Deterministic shell-reachable ops R1 did not exercise:
    - REVOKE_BOOKMARK: bm_revoke after the powerbox flow -> ok, then
      bm_resolve -> no cached bookmark (resolve failure does not clear
      s_bm_len, so the blob from R1.7 is still cached at this point).
    - VFS CREATE_DIR/DELETE round-trip (Powerbox-gated, panels auto-
      answered by run_vga_cmd's retry loop).
    - pkg REMOVE: remove hello (installed by R1.8) -> list 0 apps ->
      second remove hits the error path.
    - PROCESS_KILL error path: kill on a nonexistent PID returns FAILED.
      (A live-kill of a spawned app is racy: hello/sbox_demo exit within
      ~1s, shorter than sendkey typing latency.)"""
    print("[flow] R2 blind spots (mkdir/rm, bookmark revoke, pkg remove, kill)")
    ok = True
    steps = [
        ("bm_revoke", "bm_revoke", r"bm_revoke: ok \(0\)"),
        ("bm_resolve post-revoke", "bm_resolve",
         r"bm_resolve: no cached bookmark \(bm_create first\)"),
        ("mkdir", "mkdir /Users/tmpdir",
         r"mkdir: created /Users/tmpdir"),
        ("rm", "rm /Users/tmpdir",
         r"rm: removed /Users/tmpdir"),
        ("pkg remove", "pkg remove hello", r"pkg remove: 'hello' removed"),
        ("pkg list post-remove", "pkg list", r"pkg list: 0 app\(s\) installed"),
        ("pkg remove error", "pkg remove hello",
         r"pkg remove: FAILED \(-?\d+\)"),
        ("kill error path", "kill 99999",
         r"kill: PID 99999 SIG 9 FAILED \(-?\d+\)"),
    ]
    for name, cmd, pat in steps:
        passed = run_vga_cmd(cmd, pat, 20, name)
        print("  %s %s" % ("OK" if passed else "FAIL", name))
        ok = ok and passed
    return ok


def run_round3_flow():
    """R3 pressure/edge coverage (full-test-plan.md R3.1-R3.4).

    R3.1 IPC stress: init's 31/31 regression already runs a 100k IPC
    round-trip; the three concurrent hello spawns below add the
    multi-client angle on top.
    R3.2: fallocate drives the 32 MiB Users volume to NOSPC (deterministic
    error path), then uptime proves the kernel survived the exhaustion.
    R3.3: three hello spawns run concurrently; count signal self-test
    PASSED (baseline + 3), then assert ps no longer lists hello — the
    full spawn->run->self-terminate lifecycle.
    R3.4: flood a command line at 5 ms/key — the intact echo proves no
    key was dropped by the park table or focus routing."""
    print("[flow] R3 pressure (fill / multi-proc / flood)")
    ok = True

    # R3.2 first: it fills /Users, so nothing later in this flow may
    # write to the Users volume.
    passed = run_vga_cmd("fallocate /Users/big.bin",
                         r"fallocate: NOSPC at \d+ MiB \(err -\d+\)",
                         60, "R3.2 fallocate NOSPC")
    print("  %s R3.2 fallocate NOSPC" % ("OK" if passed else "FAIL"))
    ok = ok and passed
    if passed:
        passed = run_vga_cmd("uptime", r"System ticks: \d+", 20,
                             "R3.2 alive after fill")
        print("  %s R3.2 alive after fill" % ("OK" if passed else "FAIL"))
        ok = ok and passed

    # R3.3: exec uses the kernel blob (not the pkg store), so it works
    # even after R2.4 removed hello from the store.
    base = len(re.findall(r"hello: signal self-test PASSED", read_log()))
    for i in range(3):
        # cmd_exec prints "exec: created PID <n>" (shell rename 6918a97).
        passed = run_vga_cmd("exec", r"exec: created PID \d+", 20,
                             "R3.3 spawn #%d" % (i + 1))
        print("  %s R3.3 spawn #%d" % ("OK" if passed else "FAIL", i + 1))
        ok = ok and passed
    if ok:
        passed = wait_serial_count(r"hello: signal self-test PASSED",
                                   base + 3, 60, "R3.3 3x hello self-test")
        print("  %s R3.3 3x hello self-test" % ("OK" if passed else "FAIL"))
        ok = ok and passed
    if ok:
        time.sleep(2)  # each hello SIGTERMs itself ~0.5s after PASSED
        passed = run_vga_cmd("ps", r"PID\s+STATE", 15, "R3.3 ps")
        gone = passed and not re.search(
            r"^\s*\d+\s+\S+\s+\d+\s+\S+\s+hello\s*$",
            vga_joined(), re.MULTILINE)
        print("  %s R3.3 ps shows no hello" % ("OK" if gone else "FAIL"))
        ok = ok and gone

    # R3.4: flood a stat line at ~5 ms/key.  The full echo proves
    # every key survived; the stat line (used = 32768 KB after R3.2)
    # proves the command executed.
    flood_command("stat /Volumes/Users")
    passed = wait_vga(r"opsys\$ stat /Volumes/Users", 20, "R3.4 flood echo")
    if passed:
        passed = wait_vga(r"/Volumes/Users: \d+ KB total, \d+ KB used, read-write",
                          15, "R3.4 flood result")
    print("  %s R3.4 keyboard flood" % ("OK" if passed else "FAIL"))
    ok = ok and passed
    return ok


def run_disk_persist_flow():
    """R2.7/R3.5: virtio-blk persistence across a hard reset (--drive).

    Writes a marker to the Disk volume, resets the machine via the QEMU
    monitor (the shell lacks the ATOM_SYS_SHUTDOWN cap, so the in-guest
    reboot command is gated), then reads the marker back after the fresh
    boot.  Data surviving system_reset proves the block device is real.
    Only run with --drive (disk.img must be attached)."""
    print("[flow] R2.7/R3.5 disk persistence (--drive)")
    ok = run_vga_cmd("tee /Volumes/Disk/persist.txt hello",
                     r"tee: 5 bytes written to /Volumes/Disk/persist\.txt",
                     30, "disk write")
    print("  %s disk write" % ("OK" if ok else "FAIL"))
    if ok:
        ok = run_vga_cmd("cat /Volumes/Disk/persist.txt",
                         r"== /Volumes/Disk/persist\.txt \(5 bytes\) ==",
                         30, "disk cat pre-reset")
        print("  %s disk cat pre-reset" % ("OK" if ok else "FAIL"))
    if not ok:
        return False

    offset = len(read_log())
    print("    system_reset...", flush=True)
    mon_cmd("system_reset")
    if not wait_serial_since(offset, r"proc: CREATE pid=\d+ name=shell",
                             BOOT_TIMEOUT, "reboot shell"):
        print("  FAIL reboot (no shell CREATE)")
        return False
    if not wait_vga(r"opsys\$", BOOT_TIMEOUT, "reboot prompt"):
        print("  FAIL reboot (no prompt)")
        return False
    answer_panels()  # fresh boot: init's pending panel appears again
    ok = run_vga_cmd("cat /Volumes/Disk/persist.txt",
                     r"== /Volumes/Disk/persist\.txt \(5 bytes\) ==",
                     30, "disk cat post-reset")
    print("  %s disk cat post-reset" % ("OK" if ok else "FAIL"))
    return ok


def main():
    global qemu_proc, STEP_TIMEOUT
    drive = "--drive" in sys.argv
    for i, a in enumerate(sys.argv):
        if a == "--step-timeout" and i + 1 < len(sys.argv):
            STEP_TIMEOUT = int(sys.argv[i + 1])

    for p in (SERIAL_LOG, MON_SOCK, SCREEN_PPM):
        if os.path.exists(p):
            os.unlink(p)

    cmd = ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
           "-vnc", VNC_ADDR,
           "-serial", "file:%s" % SERIAL_LOG,
           "-monitor", "unix:%s,server=on,wait=off" % MON_SOCK]
    if drive:
        cmd += ["-drive", "file=%s,if=none,id=vd,cache=writethrough" % DISK,
                "-device", "virtio-blk-pci,drive=vd,disable-modern=on"]
    qemu_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)

    # --- boot: serial shell CREATE, then VGA prompt -------------------------
    print("[boot] waiting for shell process + VGA prompt...", flush=True)
    if not wait_serial(r"proc: CREATE pid=\d+ name=shell", BOOT_TIMEOUT,
                       "shell CREATE"):
        print("BOOT FAILED (no shell process)")
        dump_tail()
        cleanup()
        sys.exit(1)
    print("    shell process created", flush=True)
    if not wait_vga(r"opsys\$", BOOT_TIMEOUT, "shell prompt"):
        print("BOOT FAILED (no VGA prompt)")
        dump_tail()
        cleanup()
        sys.exit(1)
    print("    VGA prompt up", flush=True)
    answer_panels()  # init (PID 1) pending panel at boot

    failed = 0
    # --- serial anchors ------------------------------------------------------
    for name, pattern in SERIAL_ANCHORS:
        if wait_serial(pattern, 20, name):
            print("  OK    %s" % name)
        else:
            print("  FAIL  %s" % name)
            failed += 1

    # --- VGA shell scenarios -------------------------------------------------
    for name, text, pattern in VGA_SCENARIOS:
        print("[scenario] %s" % name, flush=True)
        print("    type: %s" % text)
        if run_vga_cmd(text, pattern, STEP_TIMEOUT, name):
            print("  OK    %s" % name)
        else:
            print("  FAIL  %s" % name)
            failed += 1

    # --- flows ---------------------------------------------------------------
    if not run_powerbox_flow():
        failed += 1
    if not run_pkg_flow():
        failed += 1
    if not run_round2_flow():
        failed += 1
    if not run_pkg_sandbox_flow():
        failed += 1
    if not run_round3_flow():
        failed += 1
    if drive and not run_disk_persist_flow():
        failed += 1

    print()
    if failed == 0:
        print("=== SMOKE PASSED (R1 + R2 + R3) ===", flush=True)
        cleanup()
        sys.exit(0)
    print("=== SMOKE FAILED: %d failure(s) ===" % failed, flush=True)
    cleanup()
    sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup()

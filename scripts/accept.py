#!/usr/bin/env python3
"""OpSys Phase 2 acceptance: Powerbox-gated bookmarks + MOVE survival.

Flow (docs/vfs_design.md §8):
  1. vfs_write /Users/a.txt hello           → create the test file
  2. bm_create /Users/a.txt r               → FAILED (-105) EACCES (授权前)
  3. parse query id from perm.ui prompt line (serial mirror)
  4. perm_answer <id> y                     → ALLOWED
  5. bm_create /Users/a.txt r               → ok (授权后)
  6. bm_resolve                             → handle returned
  7. move /Users/a.txt /Users b.txt         → rename (itemID stable)
  8. bm_resolve                             → still valid (移动后仍有效)
  9. perm_revoke                            → drop grants
 10. bm_resolve                             → FAILED (-105) EACCES (撤销后)

Drives QEMU headless: serial → build/serial.log, monitor → unix socket,
keyboard input injected via `sendkey` (PS/2 scancodes → keyboard service).
"""
import os, re, signal, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, "build/opsos.iso")
SERIAL_LOG = os.path.join(REPO, "build/serial.log")
MON_SOCK = "/tmp/opsys-mon.sock"

BOOT_TIMEOUT = 180       # GRUB 10s + kernel + service spawn
STEP_TIMEOUT = 40        # per command output wait

# --- QEMU sendkey key names -------------------------------------------------
# QEMU `sendkey` semantics: ALL keys in one call are pressed together, held,
# then released at the END of the call.  So a `shift` mid-chain stays down
# for every subsequent key in that call.  To type shifted characters (`_`,
# uppercase) correctly, each such character must be its OWN sendkey call —
# its shift is released when the call ends.  Unshifted runs can be batched.
KEYMAP = {}
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[c] = [c]
for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[c] = ["shift", c.lower()]
KEYMAP.update({
    " ": ["spc"], "/": ["slash"], ".": ["dot"], "-": ["minus"],
    "_": ["shift", "minus"], "\n": ["ret"],
})
MODIFIERS = ("shift", "ctrl", "alt")

qemu_proc = None

def mon_cmd(cmd, timeout=8):
    """Send one command to the QEMU monitor over the unix socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(MON_SOCK)
    try:
        s.recv(65536)                    # drain any pending prompt
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
    """Inject one shell command line via QEMU sendkey + Enter.

    Each sendkey call is one batch of simultaneously-pressed keys (QEMU
    releases all of them when the call ends).  Characters that need a
    modifier (uppercase, '_') are sent as their own call so the shift
    cannot leak into following characters.
    """
    calls = []
    cur = []
    for c in text:
        names = KEYMAP[c]
        if any(m in names for m in MODIFIERS):
            if cur:
                calls.append(cur)
                cur = []
            calls.append(names)          # shifted char: own call
        else:
            cur.append(names[0])         # batch unshifted chars
    if cur:
        calls.append(cur)

    for call in calls:
        r = mon_cmd("sendkey %s" % "-".join(call))
        if b"unknown key" in r:
            print("FAIL: sendkey rejected a key name for %r" % text)
            sys.exit(1)
        time.sleep(0.15)                 # let the shell consume/echo
    time.sleep(0.5)
    mon_cmd("sendkey ret")

def read_log():
    try:
        with open(SERIAL_LOG, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""

def wait_for(pattern, timeout=STEP_TIMEOUT, label=""):
    """Poll the serial log for a regex match; return match or None."""
    rex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = rex.search(read_log())
        if m:
            return m
        time.sleep(0.5)
    print("TIMEOUT waiting for: %s" % (label or pattern))
    dump_tail()
    return None

def dump_tail():
    log = read_log()
    print("--- serial.log tail (last 40 lines) ---")
    for line in log.splitlines()[-40:]:
        print("  " + line)

def cleanup():
    if qemu_proc and qemu_proc.poll() is None:
        qemu_proc.terminate()
        try:
            qemu_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu_proc.kill()
    if os.path.exists(MON_SOCK):
        os.unlink(MON_SOCK)

def main():
    global qemu_proc
    if os.path.exists(SERIAL_LOG):
        os.unlink(SERIAL_LOG)
    if os.path.exists(MON_SOCK):
        os.unlink(MON_SOCK)

    qemu_proc = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
         "-display", "none",
         "-serial", "file:%s" % SERIAL_LOG,
         "-monitor", "unix:%s,server=on,wait=off" % MON_SOCK],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # --- 0. wait for the shell prompt --------------------------------------
    print("[0] waiting for shell prompt (boot)...")
    if not wait_for(r"opsys\$ ", BOOT_TIMEOUT, "shell prompt"):
        print("BOOT FAILED")
        cleanup()
        sys.exit(1)
    print("    shell is up")

    steps = []
    def step(n, label, text, pattern):
        print("[%d] %s" % (n, label))
        print("    type: %s" % text)
        type_command(text)
        m = wait_for(pattern, STEP_TIMEOUT, label)
        if not m:
            print("STEP %d FAILED" % n)
            cleanup()
            sys.exit(1)
        print("    OK  %s" % m.group(0).strip())
        return m

    # --- 1. create the test file --------------------------------------------
    step(1, "create /Users/a.txt",
         "vfs_write /Users/a.txt hello",
         r"vfs_write: 5 bytes written to /Users/a\.txt")

    # --- 2. bm_create without a grant → -105 --------------------------------
    step(2, "bm_create before auth → EACCES",
         "bm_create /Users/a.txt r",
         r"bm_create: FAILED \(-105\)")

    # --- 3. parse the query id from the perm.ui prompt line -----------------
    m = wait_for(r"perm: app 0x\w+ requests /Users/a\.txt \(R\) - perm_answer (\d+) y/n",
                 STEP_TIMEOUT, "perm.ui prompt line")
    if not m:
        cleanup()
        sys.exit(1)
    qid = m.group(1)
    print("[3] query id = %s" % qid)

    # --- 4. perm_answer y → ALLOWED -----------------------------------------
    step(4, "answer query y",
         "perm_answer %s y" % qid,
         r"perm_answer: query %s -> ALLOWED \(0\)" % qid)

    # --- 5. bm_create now succeeds ------------------------------------------
    step(5, "bm_create after auth → ok",
         "bm_create /Users/a.txt r",
         r"bm_create: ok, \d+-byte bookmark cached")

    # --- 6. bm_resolve → handle ---------------------------------------------
    m = step(6, "bm_resolve → handle",
             "bm_resolve",
             r"bm_resolve: handle -?\d+, item 'a\.txt' \(id (\d+)\), access \d+")
    item_id = m.group(1)
    print("    item id = %s" % item_id)

    # --- 7. move /Users/a.txt → /Users/b.txt --------------------------------
    step(7, "move (rename) a.txt → b.txt",
         "move /Users/a.txt /Users b.txt",
         r"move: 'b\.txt' -> item \d+")

    # --- 8. bm_resolve still valid after move -------------------------------
    m = step(8, "bm_resolve after move → still valid",
             "bm_resolve",
             r"bm_resolve: handle -?\d+, item 'b\.txt' \(id (\d+)\), access \d+")
    if m.group(1) != item_id:
        print("FAIL: item id changed after move (%s -> %s)" % (item_id, m.group(1)))
        cleanup()
        sys.exit(1)
    print("    item id stable (%s)" % item_id)

    # --- 9. perm_revoke → drop grants ---------------------------------------
    step(9, "perm_revoke",
         "perm_revoke",
         r"perm_revoke: \d+ grant\(s\) dropped")

    # --- 10. bm_resolve → -105 after revoke ---------------------------------
    step(10, "bm_resolve after revoke → EACCES",
         "bm_resolve",
         r"bm_resolve: FAILED \(-105\)")

    print()
    print("=== ACCEPTANCE PASSED ===")
    print(" 授权前 → -EACCES / 授权后 → 句柄 / 移动后仍有效 / 撤销后 → -EACCES")
    cleanup()
    sys.exit(0)

if __name__ == "__main__":
    try:
        main()
    finally:
        cleanup()

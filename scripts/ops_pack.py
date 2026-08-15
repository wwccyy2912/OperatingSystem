#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ops_pack.py - Host-side .ops application package packer/checker
Copyright (c) 2026 OpSys Project

Builds and validates .ops application packages per docs/ops_format.md
v1.0 §2 (binary layout) / §3 (manifest text) / §4 (permission atoms).

Usage:
    python3 ops_pack.py pack <elf> <manifest.txt> <out.ops>
    python3 ops_pack.py check <out.ops>

Binary layout (all integers little-endian, no alignment):
    offset  size           field
    0       4              magic = 0x3153504F ("OPS1" little-endian)
    4       4              version = 1
    8       4              manifest_len (u32, <= 512)
    12      4              payload_len  (u32, > 0)
    16      manifest_len   manifest text (UTF-8, LF newlines, <=100 cols)
    16+manifest_len  payload_len      single x86_64 ELF

Manifest rules (§3, strict): one `key=value` per line, `#` comments and
empty lines ignored, every key must be known, `app_id` required with
`[a-zA-Z0-9_]{1,63}`, `permissions` = comma-separated atoms from the
closed §4 set (forbidden management atoms are rejected).
"""

import re
import struct
import sys

MAGIC = 0x3153504F          # "OPS1" little-endian
VERSION = 1
MANIFEST_MAX = 512          # PKG_MANIFEST_MAX (docs/ops_format.md §2)
HEADER_LEN = 16

# docs/ops_format.md §3: known manifest keys (all optional except app_id).
ALLOWED_KEYS = frozenset(("app_id", "app_name", "version", "entry",
                          "permissions"))
APP_ID_RE = re.compile(r"^[a-zA-Z0-9_]{1,63}$")

# docs/ops_format.md §4: closed permission-atom name set.
ATOM_NAMES = frozenset((
    "sys.set_time", "sys.set_timezone", "sys.shutdown",
    "hw.camera.capture", "hw.mic.record", "hw.gpu.high_perf",
    "hw.loc.coarse", "hw.loc.precise",
    "data.docs.read", "data.docs.write", "data.dl.write",
    "data.app.container.read", "data.sys.logs.read",
    "bookmark.resolve",
    "net.bind", "net.connect", "net.wifi.scan", "net.wifi.set",
    "pkg.install",
))
# docs/ops_format.md §4: management atoms an app may never declare.
FORBIDDEN_ATOMS = frozenset(("service.manage", "cap.grant_self", "sys.debug"))


def parse_manifest(text):
    """Strictly parse manifest text (docs/ops_format.md §3).

    Returns an ordered dict of {key: value}.  Raises ValueError with a
    precise message on any violation (unknown key, bad app_id, unknown
    or forbidden permission atom, line > 100 columns).
    """
    out = {}
    for lineno, raw in enumerate(text.split("\n"), start=1):
        line = raw.rstrip("\r")
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if len(line) > 100:
            raise ValueError("line %d: %d columns (max 100)" %
                             (lineno, len(line)))
        if "=" not in line:
            raise ValueError("line %d: missing '=' in '%s'" % (lineno, line))
        key, _, value = line.partition("=")
        key = key.strip()
        if key not in ALLOWED_KEYS:
            raise ValueError("line %d: unknown key '%s'" % (lineno, key))
        if key in out:
            raise ValueError("line %d: duplicate key '%s'" % (lineno, key))
        out[key] = value.strip()

    if "app_id" not in out:
        raise ValueError("manifest missing required key 'app_id'")
    if not APP_ID_RE.match(out["app_id"]):
        raise ValueError("app_id '%s' must match [a-zA-Z0-9_]{1,63}" %
                         out["app_id"])

    perms = out.get("permissions", "")
    for atom in perms.split(","):
        atom = atom.strip()
        if not atom:
            continue
        if atom in FORBIDDEN_ATOMS:
            raise ValueError("permission '%s' is forbidden for apps" % atom)
        if atom not in ATOM_NAMES:
            raise ValueError("permission '%s' is not in the closed atom set"
                             % atom)
    return out


def cmd_pack(args):
    if len(args) != 3:
        sys.stderr.write("usage: ops_pack.py pack <elf> <manifest.txt> "
                         "<out.ops>\n")
        return 1
    elf_path, manifest_path, out_path = args

    with open(manifest_path, "rb") as f:
        manifest = f.read()
    if len(manifest) > MANIFEST_MAX:
        sys.stderr.write("error: manifest is %d bytes (max %d)\n" %
                         (len(manifest), MANIFEST_MAX))
        return 1
    try:
        m = parse_manifest(manifest.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        sys.stderr.write("error: invalid manifest: %s\n" % e)
        return 1

    with open(elf_path, "rb") as f:
        payload = f.read()
    if len(payload) == 0:
        sys.stderr.write("error: payload %s is empty\n" % elf_path)
        return 1

    header = struct.pack("<IIII", MAGIC, VERSION, len(manifest),
                         len(payload))
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(manifest)
        f.write(payload)

    print("packed %s:" % out_path)
    print("  app_id       %s" % m.get("app_id"))
    print("  app_name     %s" % m.get("app_name", m["app_id"]))
    print("  version      %s" % m.get("version", "1.0"))
    print("  permissions  %s" % (m.get("permissions") or "(none)"))
    print("  manifest_len %d" % len(manifest))
    print("  payload_len  %d" % len(payload))
    return 0


def cmd_check(args):
    if len(args) != 1:
        sys.stderr.write("usage: ops_pack.py check <out.ops>\n")
        return 1
    path = args[0]

    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER_LEN:
        sys.stderr.write("error: %s too short (%d bytes, need header %d)\n" %
                         (path, len(data), HEADER_LEN))
        return 1

    magic, version, manifest_len, payload_len = struct.unpack(
        "<IIII", data[:HEADER_LEN])
    errors = []

    if magic != MAGIC:
        errors.append("bad magic 0x%08X (expected 0x%08X)" % (magic, MAGIC))
    if version != VERSION:
        errors.append("unsupported version %d (expected %d)" %
                      (version, VERSION))
    if manifest_len > MANIFEST_MAX:
        errors.append("manifest_len %d exceeds max %d" %
                      (manifest_len, MANIFEST_MAX))
    if payload_len == 0:
        errors.append("payload_len is 0")
    if len(data) != HEADER_LEN + manifest_len + payload_len:
        errors.append("file size %d != header %d + manifest %d + payload %d" %
                      (len(data), HEADER_LEN, manifest_len, payload_len))

    if errors:
        for e in errors:
            sys.stderr.write("error: %s\n" % e)
        return 1

    manifest = data[HEADER_LEN:HEADER_LEN + manifest_len]
    payload = data[HEADER_LEN + manifest_len:]
    try:
        m = parse_manifest(manifest.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        sys.stderr.write("error: invalid manifest: %s\n" % e)
        return 1

    # Payload sanity: must look like an x86_64 ELF (magic 0x464C457F).
    if len(payload) < 4 or payload[:4] != b"\x7fELF":
        sys.stderr.write("error: payload does not start with ELF magic\n")
        return 1

    print("check %s: OK" % path)
    print("  magic        0x%08X (\"OPS1\")" % magic)
    print("  version      %d" % version)
    print("  manifest_len %d" % manifest_len)
    print("  payload_len  %d" % payload_len)
    print("  total size   %d" % len(data))
    print("  app_id       %s" % m.get("app_id"))
    print("  app_name     %s" % m.get("app_name", m["app_id"]))
    print("  version      %s" % m.get("version", "1.0"))
    print("  entry        %s" % m.get("entry", "main"))
    print("  permissions  %s" % (m.get("permissions") or "(none)"))
    print("  payload      ELF (%d bytes)" % len(payload))
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("pack", "check"):
        sys.stderr.write(__doc__)
        return 1
    if sys.argv[1] == "pack":
        return cmd_pack(sys.argv[2:])
    return cmd_check(sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())

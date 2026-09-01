#!/usr/bin/env bash
#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# ==============================================================================
# build.sh - Build helper for OpSys x86_64 Microkernel
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- Colors -------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

# --- Defaults -----------------------------------------------------------------
TARGET="all"
VERBOSE=0
JOBS="$(nproc 2>/dev/null || echo 4)"

# --- Functions ----------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [TARGET]

Build the OpSys x86_64 microkernel.

Targets:
  all         Build kernel.elf (default)
  init_user   Build user-space init binary
  iso         Create bootable ISO with GRUB
  run         Build ISO and run in QEMU
  debug       Build ISO and run in QEMU with GDB stub
  clean       Remove build directory
  help        Show make help

Options:
  -v, --verbose     Show full compiler output
  -j, --jobs N      Parallel build jobs (default: $JOBS)
  -h, --help        Show this help
EOF
}

die() {
    echo -e "${RED}ERROR: $*${NC}" >&2
    exit 1
}

# --- Parse arguments ----------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) VERBOSE=1; shift ;;
        -j|--jobs)
            [[ -n "${2:-}" ]] || die "Missing value for $1"
            JOBS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "Unknown option: $1" ;;
        *)  TARGET="$1"; shift ;;
    esac
done

# --- Preflight checks ---------------------------------------------------------
cd "$PROJECT_ROOT"

for tool in "$AS" "$CC" "$LD" 2>/dev/null; do
    command -v "$tool" >/dev/null 2>&1 || true
done

# Check essential tools (warn but don't block - make will fail clearly)
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo -e "${YELLOW}WARNING: '$tool' not found in PATH${NC}" >&2
    fi
done

# --- Build --------------------------------------------------------------------
echo -e "${BOLD}>>> OpSys Build [${TARGET}]${NC}  (jobs=${JOBS})"

MAKE_ARGS=("-j${JOBS}")

if [[ "$VERBOSE" -eq 0 ]]; then
    # Suppress redundant make output; show only errors
    MAKE_ARGS+=("VERBOSE=0")
fi

if make "${MAKE_ARGS[@]}" "$TARGET"; then
    echo -e "${GREEN}>>> Build succeeded: ${TARGET}${NC}"
else
    echo -e "${RED}>>> Build failed: ${TARGET}${NC}"
    exit 1
fi

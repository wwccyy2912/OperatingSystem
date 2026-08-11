#!/usr/bin/env bash
# ==============================================================================
# run.sh - QEMU launcher for OpSys x86_64 Microkernel
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- Configuration ------------------------------------------------------------
ISO_PATH="build/opsos.iso"
QEMU="qemu-system-x86_64"
MEMORY="256M"
GDB_PORT=1234

# --- Colors -------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

# --- Defaults -----------------------------------------------------------------
DEBUG=0
SERIAL_LOG=0

# --- Functions ----------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Run the OpSys kernel in QEMU.

Options:
  --debug         Start QEMU with GDB stub (waits for connection)
  --serial        Log serial output to build/serial.log
  -h, --help      Show this help

Examples:
  $(basename "$0")              Run kernel normally
  $(basename "$0") --debug      Run with GDB stub on port $GDB_PORT
  $(basename "$0") --serial     Run and log serial to file
EOF
}

die() {
    echo -e "${RED}ERROR: $*${NC}" >&2
    exit 1
}

# --- Parse arguments ----------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   DEBUG=1; shift ;;
        --serial)  SERIAL_LOG=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown option: $1 (try --help)" ;;
    esac
done

# --- Preflight ----------------------------------------------------------------
cd "$PROJECT_ROOT"

# Check QEMU is installed
if ! command -v "$QEMU" >/dev/null 2>&1; then
    die "'$QEMU' not found. Install it: sudo apt install qemu-system-x86"
fi

# Build ISO if missing or stale
if [[ ! -f "$ISO_PATH" ]]; then
    echo -e "${YELLOW}>>> ISO not found, building...${NC}"
    make iso || die "ISO build failed"
fi

# --- Assemble QEMU command ----------------------------------------------------
QEMU_ARGS=(
    -cdrom "$ISO_PATH"
    -m "$MEMORY"
    -nographic
    -serial mon:stdio
    -d int,cpu_reset,guest_errors
)

if [[ "$SERIAL_LOG" -eq 1 ]]; then
    QEMU_ARGS+=(-serial file:build/serial.log)
    echo -e "${BOLD}>>> Serial output logged to build/serial.log${NC}"
fi

if [[ "$DEBUG" -eq 1 ]]; then
    QEMU_ARGS+=(-s -S)
    cat <<EOF

${BOLD}============================================${NC}
${BOLD}  QEMU GDB Stub Active${NC}
${BOLD}============================================${NC}

  Connect GDB in another terminal:

    ${GREEN}gdb kernel.elf${NC}

  Then in GDB:

    (gdb) ${GREEN}target remote :${GDB_PORT}${NC}
    (gdb) ${GREEN}break kernel_main${NC}
    (gdb) ${GREEN}continue${NC}

  Useful GDB commands:
    (gdb) stepi          Step one instruction
    (gdb) step           Step one source line
    (gdb) continue       Resume execution
    (gdb) info registers Show CPU registers
    (gdb) x/10i \$pc     Disassemble from PC

${BOLD}============================================${NC}

EOF
fi

# --- Launch -------------------------------------------------------------------
echo -e "${BOLD}>>> Starting QEMU${NC}"
echo -e "    ISO:    ${ISO_PATH}"
echo -e "    Memory: ${MEMORY}"
echo -e "    Serial: stdio"
if [[ "$DEBUG" -eq 1 ]]; then
    echo -e "    GDB:    port ${GDB_PORT} (waiting for connection)"
fi
echo ""

exec "$QEMU" "${QEMU_ARGS[@]}"

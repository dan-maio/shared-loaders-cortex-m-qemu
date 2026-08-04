#!/usr/bin/env bash
# qemu-gdb.sh — run one Cortex-M4 firmware under qemu-system-arm with a GDB
# stub and attach arm-none-eabi-gdb for interactive source-level debugging.
# Semihosting (console + .so file I/O) and the semihosting command line still
# work while gdb is attached.
#
# Usage:
#   qemu-gdb.sh [--arg <str>]... [--machine <name>] <firmware.elf>
#
# Options (subset of qemu-run.sh):
#   --machine <name>   QEMU machine (default netduinoplus2, an STM32F411RE)
#   --arg <str>        semihosting command-line argument; repeatable.
#   --gdb <path>       arm-none-eabi-gdb binary (default: from tools-config)
#   --port <n>         gdbstub TCP port (default 3333)
#   -h|--help          this text
#
# Example:
#   test/tools/qemu-gdb.sh --arg -c --arg c1.so \
#       build-dyn/test-dynamic-nolib/test3/scl-fw-flatfs
#   (gdb) break fw::fs_add
#   (gdb) continue
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

QEMU="${QEMU:-$(command -v qemu-system-arm || echo qemu-system-arm)}"
GDB="${ARM_GDB:-$(command -v arm-none-eabi-gdb || echo arm-none-eabi-gdb)}"
MACHINE="netduinoplus2"
PORT=3333
ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --machine) MACHINE="$2"; shift 2 ;;
        --arg)     ARGS+=("$2"); shift 2 ;;
        --gdb)     GDB="$2"; shift 2 ;;
        --port)    PORT="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^# Example/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         ELF="$1"; shift ;;
    esac
done

[[ -n "${ELF:-}" && -f "$ELF" ]] || { echo "error: firmware ELF not found" >&2; exit 2; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "error: qemu-system-arm not found (set QEMU=...)" >&2; exit 2; }
command -v "$GDB"  >/dev/null 2>&1 || { echo "error: arm-none-eabi-gdb not found (set ARM_GDB=...)" >&2; exit 2; }

SHCFG="enable=on,target=native"
for a in "${ARGS[@]}"; do SHCFG+=",arg=${a}"; done

echo "== qemu-gdb: $ELF  (machine $MACHINE, gdbstub tcp::$PORT)"
echo "== qemu stderr -> $ELF.qemu-gdb.log"
echo "== in gdb: 'target remote :$PORT' is pre-loaded; 'continue' to start."

"$QEMU" -M "$MACHINE" -display none -monitor none -serial none \
        -semihosting-config "$SHCFG" \
        -gdb "tcp::${PORT}" \
        -S \
        -kernel "$ELF" >"${ELF}.qemu-gdb.log" 2>&1 &
QPID=$!
trap 'kill "$QPID" 2>/dev/null || true' EXIT

# -S halts the CPU at reset so gdb can set breakpoints before execution; gdb's
# 'continue' then lets the firmware run (semihosting works normally).
"$GDB" "$ELF" -q \
    -ex "set pagination off" \
    -ex "target remote :${PORT}" \
    -ex "continue"

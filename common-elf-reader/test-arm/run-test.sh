#!/usr/bin/env bash
# run-test.sh — flash the elf-reader-test-arm binary, run it via semihosting,
# pass the .elf fixture path on the semihosting cmdline, and report pass/fail.
#
# Usage:
#   ./run-test.sh [build-dir]      # default: build
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-${SCRIPT_DIR}/build}"
OCDCFG="${SCRIPT_DIR}/openocd.cfg"

# Find OpenOCD: PATH first, then xPacks default.
if command -v openocd &>/dev/null; then
    OPENOCD="openocd"
else
    OPENOCD="${OPENOCD:-$HOME/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin/openocd}"
fi

# CMake copies the stripped .elf as "411ce-100Mhz.elf" into the build dir.
# Use it for both flashing and as the parser fixture.
if   [ -f "${BUILD_DIR}/elf-reader-test-arm.elf" ]; then
    ELF="${BUILD_DIR}/elf-reader-test-arm.elf"
elif [ -f "${BUILD_DIR}/elf-reader-test-arm" ]; then
    ELF="${BUILD_DIR}/elf-reader-test-arm"
else
    echo "ERROR: elf-reader-test-arm not found in ${BUILD_DIR}. Build first:" >&2
    echo "  ./build.sh" >&2
    exit 1
fi
FIXTURE="${BUILD_DIR}/411ce-100Mhz.elf"
if [ ! -f "$FIXTURE" ]; then
    echo "ERROR: fixture $FIXTURE not found" >&2
    exit 1
fi

echo "=== Flashing ${ELF} ==="
echo "=== Fixture: ${FIXTURE} ==="

# Semihosting cmdline: argv[0] = program name, argv[1] = ELF fixture filename.
# OpenOCD runs from BUILD_DIR so semihost_open resolves the fixture by name.
# Flash the .hex (small, fast), test parses the stripped .elf via semihosting.
( cd "$BUILD_DIR" && "$OPENOCD" -f "$OCDCFG" \
    -c "reset halt" \
    -c "arm semihosting enable" \
    -c "arm semihosting_cmdline elf-reader-test-arm 411ce-100Mhz.elf" \
    -c "flash write_image erase ${ELF}" \
    -c "reset halt" \
    -c "resume" \
    -c "wait_halt 60000" \
    -c "shutdown" 2>&1 ) | tee "${BUILD_DIR}/test-arm-output.txt"

# The test prints "N passed, M failed" via semihosting. The "timed out" from
# wait_halt is expected (the test spins after exit). Check for "0 failed".
if grep -q "passed," "${BUILD_DIR}/test-arm-output.txt" && ! grep -q "[1-9][0-9]* failed" "${BUILD_DIR}/test-arm-output.txt"; then
    echo ""
    echo "=== TEST PASSED ==="
    exit 0
else
    echo ""
    echo "=== TEST FAILED (see ${BUILD_DIR}/test-arm-output.txt) ==="
    exit 1
fi

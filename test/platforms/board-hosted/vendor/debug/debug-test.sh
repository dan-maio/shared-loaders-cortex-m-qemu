#!/bin/bash
# Run test under OpenOCD (capture semihosting output) — OpenOCD only (no GDB)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"
TEST_BIN="$PROJECTS/test/build/run_tests"
CFG="$(cd "$SCRIPT_DIR/../../../board" && pwd)/openocd-${SCL_BOARD:-weact}.cfg"
SCRIPTS="/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts"

if [ ! -f "$TEST_BIN" ]; then
    make -C "$PROJECTS/test" build/run_tests
fi

cd "$PROJECTS"
killall -s 9 -q openocd 2>/dev/null || true; sleep 1
exec openocd -s "$SCRIPTS" -f "$CFG" \
    -c "init" \
    -c "reset halt" \
    -c "program $TEST_BIN" \
    -c "arm semihosting enable" \
    -c "reset run"

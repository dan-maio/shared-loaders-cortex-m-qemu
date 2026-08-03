#!/bin/bash
# Run test under valgrind (memory check)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"
TEST_BIN="$PROJECTS/test/build/run_tests"

if [ ! -f "$TEST_BIN" ]; then
    make -C "$PROJECTS/test" build/run_tests
fi

cd "$PROJECTS/../f411re-dynloader"
valgrind --leak-check=full "$TEST_BIN" 2>&1

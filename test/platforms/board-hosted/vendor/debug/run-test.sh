#!/bin/bash
# Run a specific test by name
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECTS="$(dirname "$SCRIPT_DIR")"
FILTER="${1:-*}"

TEST_BIN="$PROJECTS/test/build/run_tests"
if [ ! -f "$TEST_BIN" ]; then
    make -C "$PROJECTS/test" build/run_tests
fi

cd "$PROJECTS/../f411re-dynloader"
"$TEST_BIN" -tc "$FILTER"

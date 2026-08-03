#!/usr/bin/env bash
# build.sh — configure and build the elf-reader-test-arm binary.
#
# Usage:
#   ./build.sh [build-dir]    # default: build
#
# Works regardless of where the project is cloned — stale CMake caches
# from a previous location are automatically discarded.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-${SCRIPT_DIR}/build}"

CACHE="${BUILD_DIR}/CMakeCache.txt"
if [ -f "$CACHE" ]; then
    # If the source dir in the cache doesn't match, nuke it.
    CACHED_SRC=$(grep -m1 'CMAKE_HOME_DIRECTORY' "$CACHE" 2>/dev/null | cut -d= -f2 || true)
    if [ "$CACHED_SRC" != "$SCRIPT_DIR" ]; then
        echo "Source dir changed ($CACHED_SRC -> $SCRIPT_DIR), clearing cache."
        rm -f "$CACHE" "$BUILD_DIR/CMakeFiles"/cmake.check_cache 2>/dev/null || true
    fi
fi

cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/cmake/arm-none-eabi.cmake"

cmake --build "$BUILD_DIR"

# Find the produced binary (CMake may or may not append .elf).
if   [ -f "${BUILD_DIR}/elf-reader-test-arm.elf" ]; then
    BIN="${BUILD_DIR}/elf-reader-test-arm.elf"
else
    BIN="${BUILD_DIR}/elf-reader-test-arm"
fi
echo ""
echo "=== Build complete: ${BIN} ==="
echo "    Flash + run: ./run-test.sh ${BUILD_DIR}"

#!/usr/bin/env bash
# Remove every build tree in the workspace.
#
# Covers the shared cmake trees, the per-test `build/` directories (the newer
# test projects configure in place), and the fixture builds that are still
# driven by their own Makefiles.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

echo "=== shared cmake trees ==="
rm -rf test/build-fw test/build-dynamic-newlib test/build-dynamic-newlib-hf \
       test-dynamic/build test-overlay/build \
       test/build-static test-relocatable/build test-xip/build test-xip/build-newlib

echo "=== per-test build dirs ==="
for d in common-elf-reader common-elf-reader/test-arm \
         test-dynamic/test-dynamic-nolib/test1 test-dynamic/test-dynamic-nolib/test2 \
         test-dynamic/test-dynamic-nolib/test3 test-dynamic/test-dynamic-nolib/test4 \
         test-dynamic/test-dynamic-newlib/test1 test-dynamic/test-dynamic-newlib/test2 \
         test-overlay/test1 test-static/test-static test-static/test-static-cpp \
         test-relocatable test-static/test-static-direct test-static/test-drv \
         test-xip/test-xip-dynamic-nolib/test1 \
         test-xip/test-xip-dynamic-newlib/test1 \
         test-xip/test-xip-static/semihosting test-xip/test-xip-static/spi-flash \
         test-xip/test-xip-static-direct \
         test-xip/test-xip-interrupt/test1 test-xip/test-xip-interrupt-static \
         test-xip/test-xip-interrupt-static-direct \
         ; do
    rm -rf "$d/build"
done

echo "=== fixture builds ==="
make -C test-relocatable/fixtures-reloc clean 2>/dev/null || true

echo "=== cleaned ==="

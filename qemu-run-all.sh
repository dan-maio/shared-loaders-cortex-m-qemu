#!/usr/bin/env bash
# qemu-run-all.sh — build and run every SCL test family under qemu-system-arm
# (Cortex-M4, -machine netduinoplus2), no board required.
#
# Usage:  ./qemu-run-all.sh          # build + run everything
#         ./qemu-run-all.sh --build-only
#         ./qemu-run-all.sh --no-clean
#
# Each family is configured with SCL_PLATFORM=qemu-f411re so the qemu runner
# (test/tools/qemu-run.sh) boots the firmware with semihosting; the .so files
# are delivered via the semihosting command line / semihosting file I/O, the
# flat_fs "flash" is backed by CCM SRAM, and the LED is observed via a
# semihosting trace.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

source tools-config.sh
BUILD_ONLY=0
KEEP=0
for a in "$@"; do
    case "$a" in
        --build-only) BUILD_ONLY=1 ;;
        --no-clean)   KEEP=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

cleanup() { [[ $KEEP -eq 1 ]] || rm -rf build-qemu-*; }
trap cleanup EXIT

run_family() {  # name  toolchain  source  [extra cmake args...]
    local name="$1"; shift
    local tc="$1"; shift
    local src="$1"; shift
    echo ""
    echo "############################################################"
    echo "# $name  (toolchain: $(basename "$tc"))"
    echo "############################################################"
    local bdir="build-qemu-${name}"
    cmake -S "$src" -B "$bdir" -DCMAKE_TOOLCHAIN_FILE="$HERE/$tc" -DSCL_PLATFORM=qemu-f411re "$@"
    cmake --build "$bdir" -j"$(nproc)"
    if [[ $BUILD_ONLY -eq 1 ]]; then
        echo "# $name: BUILD OK"
        return
    fi
    timeout 900 ctest --test-dir "$bdir" --output-on-failure || {
        echo "# $name: TESTS FAILED (see above)"; return 1; }
}

ok=1
run_family "dynamic-nolib" cmake/arm-none-eabi.cmake test-dynamic            || ok=0
run_family "newlib"        cmake/arm-none-eabi-hosted.cmake test-dynamic/test-dynamic-newlib || ok=0
run_family "overlay"       cmake/arm-none-eabi.cmake test-overlay            || ok=0
run_family "relocatable"   cmake/arm-none-eabi.cmake test-relocatable        || ok=0
run_family "xip-nolib"     cmake/arm-none-eabi.cmake test-xip/test-xip-dynamic-nolib/test1 || ok=0
run_family "xip-static"    cmake/arm-none-eabi.cmake test-xip/test-xip-static/semihosting || ok=0
run_family "xip-static-direct" cmake/arm-none-eabi.cmake test-xip/test-xip-static-direct || ok=0
run_family "xip-interrupt" cmake/arm-none-eabi.cmake test-xip/test-xip-interrupt/test1 || ok=0
run_family "xip-int-static" cmake/arm-none-eabi.cmake test-xip/test-xip-interrupt-static || ok=0
run_family "xip-int-static-direct" cmake/arm-none-eabi.cmake test-xip/test-xip-interrupt-static-direct || ok=0

# The hosted XIP newlib app loads hard-float fixtures (cpp3.so), so the
# firmware must be built hard-float too (SCL_HF=ON).
run_family "xip-newlib" cmake/arm-none-eabi-hosted.cmake test-xip/test-xip-dynamic-newlib/test1 -DSCL_HF=ON || ok=0

if [[ $ok -eq 1 ]]; then
    echo ""
    echo "============================================================"
    echo "ALL QEMU TEST FAMILIES PASSED"
    echo "============================================================"
else
    echo ""
    echo "============================================================"
    echo "SOME QEMU TEST FAMILIES FAILED (see above)"
    echo "============================================================"
    exit 1
fi

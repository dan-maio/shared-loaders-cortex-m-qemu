#!/usr/bin/env bash
# qemu-run-all.sh — build and run every SCL test family under qemu-system-arm
# (Cortex-M4, -machine netduinoplus2), no board required.
#
# Usage:  ./qemu-run-all.sh          # build + run everything
#         ./qemu-run-all.sh --no-clean
#
# Each family is dispatched through the per-tree build-and-run.sh scripts with
# board=qemu, which configure with SCL_PLATFORM=qemu-f411re and run via
# qemu-run.sh (not OpenOCD).
#
# SPI-dependent tests are skipped (QEMU has no SPI model).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

source tools-config.sh
KEEP=0
for a in "$@"; do
    case "$a" in
        --no-clean) KEEP=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

run_test() {  # name  script  [args...]
    local name="$1"; shift
    echo ""
    echo "############################################################"
    echo "# $name"
    echo "############################################################"
    timeout 300 "$@" && echo "# $name: PASS" || {
        local rc=$?
        echo "# $name: FAIL (exit $rc)"
        return 1
    }
}

ok=1

# --- Dynamic freestanding (nolib) ---
run_test "dyn1"          ./test-dynamic/build-and-run.sh qemu dynamic-nolib/test1   || ok=0
run_test "dyn2"          ./test-dynamic/build-and-run.sh qemu dynamic-nolib/test2   || ok=0
run_test "flatfs"        ./test-dynamic/build-and-run.sh qemu dynamic-nolib/test3   || ok=0
run_test "gotplt-demo"   ./test-dynamic/build-and-run.sh qemu dynamic-nolib/test4   || ok=0

# --- Dynamic hosted (newlib) ---
run_test "newlib1-sf"    ./test-dynamic/build-and-run.sh qemu dynamic-newlib/test1  || ok=0
run_test "newlib2-sf"    ./test-dynamic/build-and-run.sh qemu dynamic-newlib/test2  || ok=0

# --- Overlay ---
run_test "ovl1"          ./test-overlay/build-and-run.sh qemu test1                 || ok=0

# --- Relocatable ---
run_test "reloc"         ./test-relocatable/build-and-run.sh qemu reloc             || ok=0
run_test "classic"       ./test-relocatable/build-and-run.sh qemu classic           || ok=0
run_test "interrupt"     ./test-relocatable/build-and-run.sh qemu interrupt         || ok=0

# --- XIP ---
run_test "xip-nolib"     ./test-xip/build-and-run.sh qemu dynamic-nolib            || ok=0
run_test "xip-newlib"    ./test-xip/build-and-run.sh qemu dynamic-newlib           || ok=0
run_test "xip-interrupt" ./test-xip/build-and-run.sh qemu interrupt                || ok=0
run_test "xip-static"    ./test-xip/build-and-run.sh qemu static/semihosting       || ok=0
run_test "xip-static-direct" ./test-xip/build-and-run.sh qemu static-direct        || ok=0
run_test "xip-int-static" ./test-xip/build-and-run.sh qemu interrupt-static        || ok=0
run_test "xip-int-static-direct" ./test-xip/build-and-run.sh qemu interrupt-static-direct || ok=0

# SPI tests are WEACT-only (QEMU has no SPI model) — skipped:
#   flatfs-spi, flatfs-bitbang-spi, drv, classic-spi, static/spi-flash

if [[ $ok -eq 1 ]]; then
    echo ""
    echo "============================================================"
    echo "ALL QEMU TESTS PASSED"
    echo "============================================================"
else
    echo ""
    echo "============================================================"
    echo "SOME QEMU TESTS FAILED (see above)"
    echo "============================================================"
    exit 1
fi

#!/usr/bin/env bash
# run-all-boards.sh — run every SCL test family on real hardware (nucleo + weact)
# by dispatching to each per-tree build-and-run.sh. Polls each test's log every
# 40s (printing the tail so progress is visible), allows max 2 retries, and
# killall -s 9 openocd between attempts. SPI tests are WEACT-only.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
source tools-config.sh
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"

LOGDIR="$HERE/build-all-boards-logs"
mkdir -p "$LOGDIR"
POLL_S=40
MAX_RETRY=2
MAX_ATTEMPT_S=900
RUN_MS=600000

BOARDS=(nucleo weact)

run_test() {  # name marker script target weact-only(0|1)
    local name="$1" marker="$2" script="$3" target="$4" wonly="$5"
    local board="$6"
    local log="$LOGDIR/$board-$name.log"
    local attempts=$((MAX_RETRY + 1))
    local pid rc found exited

    if [[ "$wonly" == "1" && "$board" != "weact" ]]; then
        echo "== [$board] $name: SKIP (SPI only)"
        return 0
    fi

    echo ""
    echo "############################################################"
    echo "# [$board] $name   ($script $board $target)"
    echo "############################################################"

    for i in $(seq 1 "$attempts"); do
        [[ "$i" == 1 ]] || echo "--- retry $((i-1)) for $name ---"
        killall -s 9 openocd 2>/dev/null || true
        sleep 1
        found=""; exited=""
        RUN_MS="$RUN_MS" setsid stdbuf -oL timeout -k 30 "$MAX_ATTEMPT_S" "$script" "$board" "$target" >"$log" 2>&1 &
        pid=$!
        for step in $(seq 1 $(( MAX_ATTEMPT_S / POLL_S ))); do
            sleep "$POLL_S"
            if grep -qE "$marker" "$log" 2>/dev/null; then found=1; break; fi
            kill -0 "$pid" 2>/dev/null || { exited=1; break; }
            echo "--- [$board] $name attempt $i still running (elapsed $((step * POLL_S))s); tail:"
            tail -5 "$log" 2>/dev/null | sed 's/^/    | /'
        done
        if [[ -z "$found" ]]; then
            sleep 1
            if grep -qE "$marker" "$log" 2>/dev/null; then found=1; fi
        fi
        kill -TERM -- -"$pid" 2>/dev/null || true
        sleep 1
        kill -KILL -- -"$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null; rc=$?
        killall -s 9 openocd 2>/dev/null || true

        if [[ -n "$found" ]] || { [[ -n "$exited" && "$rc" == 0 ]]; }; then
            echo "# [$board] $name: PASS (attempt $i)"
            return 0
        fi
        echo "# [$board] $name: attempt $i FAILED (rc=${rc:-killed}, no marker \"$marker\")"
        tail -8 "$log" 2>/dev/null | sed 's/^/    | /'
    done
    echo "# [$board] $name: FAIL after $attempts attempts"
    return 1
}

ok=1
for BOARD in "${BOARDS[@]}"; do
    export SCL_BOARD="$BOARD"

    run_test "dyn1"        "4b: done"        ./test-dynamic/build-and-run.sh    dynamic-nolib/test1  0 "$BOARD" || ok=0
    run_test "dyn2"        "battery: done"   ./test-dynamic/build-and-run.sh    dynamic-nolib/test2  0 "$BOARD" || ok=0
    run_test "flatfs"      "battery: done"   ./test-dynamic/build-and-run.sh    dynamic-nolib/test3  0 "$BOARD" || ok=0
    run_test "gotplt-demo" "RESULT: ALL PASS" ./test-dynamic/build-and-run.sh   dynamic-nolib/test4  0 "$BOARD" || ok=0
    run_test "newlib1-sf"  "-> SUCCESS"      ./test-dynamic/build-and-run.sh    dynamic-newlib/test1 0 "$BOARD" || ok=0
    run_test "newlib2-sf"  "ALL OK"          ./test-dynamic/build-and-run.sh    dynamic-newlib/test2 0 "$BOARD" || ok=0
    run_test "flatfs-spi"  "battery: done"   ./test-dynamic/build-and-run.sh    flatfs-spi           1 "$BOARD" || ok=0
    run_test "flatfs-bitbang" "battery: done" ./test-dynamic/build-and-run.sh   flatfs-bitbang-spi   1 "$BOARD" || ok=0
    run_test "ovl1"        "overlay: done"   ./test-overlay/build-and-run.sh    test1                0 "$BOARD" || ok=0
    run_test "reloc"       "RESULT: ALL PASS" ./test-relocatable/build-and-run.sh reloc              0 "$BOARD" || ok=0
    run_test "classic"     "==== classic demo complete ====" ./test-relocatable/build-and-run.sh classic 0 "$BOARD" || ok=0
    run_test "classic-spi" "==== classic-spi demo complete ====" ./test-relocatable/build-and-run.sh classic-spi 1 "$BOARD" || ok=0
    run_test "interrupt"   "RESULT: ALL PASS" ./test-relocatable/build-and-run.sh interrupt          0 "$BOARD" || ok=0
    run_test "static"      "Second 1"        ./test-static/build-and-run.sh     static               0 "$BOARD" || ok=0
    run_test "static-cpp"  "Second 1"        ./test-static/build-and-run.sh     static-cpp           0 "$BOARD" || ok=0
    run_test "static-direct" "All direct-link static loader tests completed successfully" ./test-static/build-and-run.sh static-direct 0 "$BOARD" || ok=0
    run_test "drv"         "test-drv done"   ./test-static/build-and-run.sh     drv                  1 "$BOARD" || ok=0
    run_test "xip-nolib"   "scl-fw-xip-nolib1: done" ./test-xip/build-and-run.sh dynamic-nolib       0 "$BOARD" || ok=0
    run_test "xip-newlib"  "scl-fw-xip-newlib1: done" ./test-xip/build-and-run.sh dynamic-newlib     0 "$BOARD" || ok=0
    run_test "xip-interrupt" "regular tests done" ./test-xip/build-and-run.sh   interrupt            0 "$BOARD" || ok=0
    run_test "xip-static"  "scl-fw-xip-static: done" ./test-xip/build-and-run.sh static/semihosting  0 "$BOARD" || ok=0
    run_test "xip-spi-flash" "test PASSED"    ./test-xip/build-and-run.sh       static/spi-flash     1 "$BOARD" || ok=0
    run_test "xip-static-direct" "scl-fw-xip-static-direct: done" ./test-xip/build-and-run.sh static-direct 0 "$BOARD" || ok=0
    run_test "xip-int-static" "regular tests done" ./test-xip/build-and-run.sh  interrupt-static     0 "$BOARD" || ok=0
    run_test "xip-int-static-direct" "regular tests done" ./test-xip/build-and-run.sh interrupt-static-direct 0 "$BOARD" || ok=0
done

if [[ $ok -eq 1 ]]; then
    echo ""
    echo "============================================================"
    echo "ALL BOARD TESTS PASSED"
    echo "============================================================"
else
    echo ""
    echo "============================================================"
    echo "SOME BOARD TESTS FAILED (see above and $LOGDIR)"
    echo "============================================================"
    exit 1
fi

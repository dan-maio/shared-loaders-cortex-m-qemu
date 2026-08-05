#!/usr/bin/env bash
# weact-run-all.sh — build, flash and run every SCL test family on the WeAct
# board (WEACT-F411CE via its on-board CMSIS-DAP WCH-Link probe).
#
# Usage:  ./weact-run-all.sh          # run everything (weact)
#         ./weact-run-all.sh --skip <name>...   # skip already-passed tests
#
# Each family is dispatched through the per-tree build-and-run.sh scripts with
# board=weact. Each test is launched (with a generous RUN_MS cap so flash-run
# auto-shuts OpenOCD down) and its semihosting console log is polled for the
# firmware's completion marker; as soon as the marker appears the test run is
# torn down and counted PASS. A test that fails is retried once.
# Logs are saved under build-weact-logs/.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

SKIP_LIST="${SKIP_LIST:-}"
if [[ "${1:-}" == "--skip" ]]; then
    shift
    SKIP_LIST="$*"
fi

source tools-config.sh
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
export SCL_BOARD=weact

LOGDIR="$HERE/build-weact-logs"
mkdir -p "$LOGDIR"

# Max wall time per attempt: build allowance + firmware run (ms). The firmware
# run itself is capped by RUN_MS (auto-shutdown), and we kill early on marker.
MAX_ATTEMPT_S=900
RUN_MS=600000

run_test() {  # name  marker  script [args...]
    local name="$1"; shift
    local marker="$1"; shift
    local script="$1"; shift
    local log="$LOGDIR/$name.log"
    local attempts=2   # 1 initial + 1 retry
    local pid rc found exited

    echo ""
    echo "############################################################"
    echo "# $name   ($script $*)"
    echo "############################################################"

    for s in $SKIP_LIST; do
        [[ "$name" == "$s" ]] && { echo "# $name: SKIP (--skip)"; return 0; }
    done

    for i in 1 2; do
        [[ "$i" == 1 ]] || echo "--- retry for $name ---"
        killall -s 9 openocd 2>/dev/null || true
        sleep 1
        found=""; exited=""
        # stdbuf -oL line-buffers openocd's stdout (redirected to the log file)
        # so the completion marker is written as it is printed, not only when
        # openocd's 4KB stdio buffer fills or the process exits.
        RUN_MS="$RUN_MS" setsid stdbuf -oL timeout -k 30 "$MAX_ATTEMPT_S" "$script" "$@" >"$log" 2>&1 &
        pid=$!
        for _ in $(seq 1 $(( MAX_ATTEMPT_S / 2 ))); do
            sleep 2
            if grep -qE "$marker" "$log" 2>/dev/null; then found=1; break; fi
            kill -0 "$pid" 2>/dev/null || { exited=1; break; }
        done
        # Re-check once after the loop: the process may have self-exited
        # (firmware SYS_EXIT via semihosting) just as it flushed the marker.
        if [[ -z "$found" ]]; then
            sleep 1
            if grep -qE "$marker" "$log" 2>/dev/null; then found=1; fi
        fi
        # Tear the attempt down (kill the whole process group), then reap rc.
        kill -TERM -- -"$pid" 2>/dev/null || true
        sleep 1
        kill -KILL -- -"$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null; rc=$?
        killall -s 9 openocd 2>/dev/null || true

        # PASS = completion marker seen, OR the run harness self-exited cleanly
        # (firmware finished and returned 0, e.g. the hosted newlib tests that
        # main-return -> semihosting SYS_EXIT -> openocd exits 0).
        if [[ -n "$found" ]] || { [[ -n "$exited" && "$rc" == 0 ]]; }; then
            echo "# $name: PASS (attempt $i)"
            return 0
        fi
        echo "# $name: attempt $i FAILED (rc=${rc:-killed}, no marker \"$marker\")"
        tail -8 "$log" 2>/dev/null | sed 's/^/    | /'
    done
    echo "# $name: FAIL after $attempts attempts"
    return 1
}

ok=1

# --- Dynamic freestanding (nolib) ---
run_test "dyn1"             "4b: done"                ./test-dynamic/build-and-run.sh weact dynamic-nolib/test1 || ok=0
run_test "dyn2"             "battery: done"           ./test-dynamic/build-and-run.sh weact dynamic-nolib/test2 || ok=0
run_test "flatfs"           "battery: done"           ./test-dynamic/build-and-run.sh weact dynamic-nolib/test3 || ok=0
run_test "gotplt-demo"      "RESULT: ALL PASS"        ./test-dynamic/build-and-run.sh weact dynamic-nolib/test4 || ok=0

# --- Dynamic hosted (newlib) ---
run_test "newlib1-sf"       "-> SUCCESS"              ./test-dynamic/build-and-run.sh weact dynamic-newlib/test1 || ok=0
run_test "newlib2-sf"       "ALL OK"                  ./test-dynamic/build-and-run.sh weact dynamic-newlib/test2 || ok=0

# --- Dynamic SPI flatfs (weact only) ---
run_test "flatfs-spi"      "battery: done"           ./test-dynamic/build-and-run.sh weact flatfs-spi || ok=0
run_test "flatfs-bitbang"  "battery: done"           ./test-dynamic/build-and-run.sh weact flatfs-bitbang-spi || ok=0

# --- Overlay ---
run_test "ovl1"             "overlay: done"           ./test-overlay/build-and-run.sh weact test1 || ok=0

# --- Relocatable ---
run_test "reloc"            "RESULT: ALL PASS"        ./test-relocatable/build-and-run.sh weact reloc || ok=0
run_test "classic"          "==== classic demo complete ====" ./test-relocatable/build-and-run.sh weact classic || ok=0
run_test "classic-spi"     "==== classic-spi demo complete ====" ./test-relocatable/build-and-run.sh weact classic-spi || ok=0
run_test "interrupt"        "RESULT: ALL PASS"        ./test-relocatable/build-and-run.sh weact interrupt || ok=0

# --- Static loader ---
run_test "static"           "Second 1"                ./test-static/build-and-run.sh weact static || ok=0
run_test "static-cpp"       "Second 1"                ./test-static/build-and-run.sh weact static-cpp || ok=0
run_test "static-direct"    "All direct-link static loader tests completed successfully" ./test-static/build-and-run.sh weact static-direct || ok=0
run_test "drv"             "test-drv done"           ./test-static/build-and-run.sh weact drv || ok=0

# --- XIP ---
run_test "xip-nolib"       "scl-fw-xip-nolib1: done" ./test-xip/build-and-run.sh weact dynamic-nolib || ok=0
run_test "xip-newlib"      "scl-fw-xip-newlib1: done" ./test-xip/build-and-run.sh weact dynamic-newlib || ok=0
run_test "xip-interrupt"   "regular tests done"      ./test-xip/build-and-run.sh weact interrupt || ok=0
run_test "xip-static"      "scl-fw-xip-static: done" ./test-xip/build-and-run.sh weact static/semihosting || ok=0
run_test "xip-spi-flash"   "test PASSED"             ./test-xip/build-and-run.sh weact static/spi-flash || ok=0
run_test "xip-static-direct" "scl-fw-xip-static-direct: done" ./test-xip/build-and-run.sh weact static-direct || ok=0
run_test "xip-int-static"  "regular tests done"      ./test-xip/build-and-run.sh weact interrupt-static || ok=0
run_test "xip-int-static-direct" "regular tests done" ./test-xip/build-and-run.sh weact interrupt-static-direct || ok=0

if [[ $ok -eq 1 ]]; then
    echo ""
    echo "============================================================"
    echo "ALL WEACT TESTS PASSED"
    echo "============================================================"
else
    echo ""
    echo "============================================================"
    echo "SOME WEACT TESTS FAILED (see above and $LOGDIR)"
    echo "============================================================"
    exit 1
fi

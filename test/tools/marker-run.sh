#!/usr/bin/env bash
# marker-run.sh — run ONE SCL test on a board with marker-kill + one retry.
#
# Usage: marker-run.sh <nucleo|weact> <name> <marker> <script> [args...]
#
# Launches the given per-tree build-and-run.sh (e.g. test-xip/build-and-run.sh)
# for the selected board, polls the semihosting console log for the firmware's
# completion marker, and kills OpenOCD the instant the marker appears — so a
# test finishes as soon as the firmware reports its result, with no fixed waits.
# A transient probe/connect failure is retried once.
# Prints PASS or FAIL. Log saved to <sclroot>/build-marker-logs/<name>.log.
set -uo pipefail

BOARD="$1"; shift
name="$1"; shift
marker="$1"; shift
case "$BOARD" in nucleo|weact) ;; *) echo "board must be nucleo or weact (got '$BOARD')" >&2; exit 2 ;; esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."
LOGDIR="$ROOT/build-marker-logs"
mkdir -p "$LOGDIR"
log="$LOGDIR/$name.log"

run_attempt() {
    : > "$log"
    killall -s 9 openocd 2>/dev/null || true
    sleep 1
    SCL_BOARD="$BOARD" RUN_MS=600000 setsid stdbuf -oL timeout -k 30 900 "$@" >"$log" 2>&1 &
    local pid=$!
    local found=""
    while :; do
        sleep 2
        if grep -qE "$marker" "$log" 2>/dev/null; then found=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
    done
    if [[ -z "$found" ]]; then sleep 1; grep -qE "$marker" "$log" 2>/dev/null && found=1; fi
    kill -TERM -- -"$pid" 2>/dev/null || true
    sleep 1
    kill -KILL -- -"$pid" 2>/dev/null || true
    killall -s 9 openocd 2>/dev/null
    [[ -n "$found" ]]
}

if run_attempt "$@"; then
    echo "PASS"
    exit 0
fi
echo "--- retry (transient probe error?) ---"
if run_attempt "$@"; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
tail -15 "$log"
exit 1

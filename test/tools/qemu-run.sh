#!/usr/bin/env bash
# qemu-run.sh — run one Cortex-M4 firmware test under qemu-system-arm with
# semihosting, and verify it passed. This is the QEMU counterpart of the
# OpenOCD flash-run.sh: instead of flashing a .hex to a board it boots the .elf
# on an emulated STM32F411RE (default -machine netduinoplus2) and feeds the
# firmware's semihosting command line (SYS_GET_CMDLINE) with the .so list.
#
# Usage:
#   qemu-run.sh [options] <firmware.elf>
#
# Options:
#   --machine <name>     QEMU machine (default netduinoplus2, an STM32F411RE)
#   --arg <str>          semihosting command-line argument; repeatable. The
#                        firmware sees them space-separated (the QEMU analogue
#                        of OpenOCD's `arm semihosting_cmdline <args...>`).
#   --done <marker>      completion marker; when it appears in the log the run
#                        is complete. Repeatable (any one ends the run).
#   --allow <pattern>    a FAIL/FAILED line that is EXPECTED (a negative-path
#                        test, e.g. "load_set FAILED"). Such lines do not fail
#                        the run. Repeatable.
#   --timeout <secs>     hard stop (default 120). The run also stops ~1s after
#                        a --done marker so the LED blink loop is observed.
#   --need-led <n>       require at least n "[LED:" toggle reports (default 0)
#   --need-flash <n>     require "flat_fs files = <>=n" (flash provisioning check)
#   --log <file>         save the semihosting console log here
#   --gdb                delegate to qemu-gdb.sh for interactive debugging with
#                        arm-none-eabi-gdb (qemu + gdbstub + semihosting).
#                        (Semihosting + command line still work.)
#   -h|--help            this text
#
# Exit status: 0 = pass, 1 = fail, 2 = usage/build error, 124 = timeout.
#
# Pass/fail verdict:
#   FAIL if the log shows a HardFault, a "FAILURES) ==" / "SOME FAILED" /
#   "-> FAILURE" summary, a FAILed check line, or (with --need-*) the LED/flash
#   requirements are unmet. PASS otherwise once a --done marker has appeared.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../../tools-config.sh"

QEMU="${QEMU:-$(command -v qemu-system-arm || echo qemu-system-arm)}"
MACHINE="netduinoplus2"
TIMEOUT=120
DONE=()
ARGS=()
ALLOW=()
LOG=""
NEED_LED=0
NEED_FLASH=0
GDB=0

usage() { sed -n '2,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//' >&2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --machine)  MACHINE="$2"; shift 2 ;;
        --arg)      ARGS+=("$2"); shift 2 ;;
        --done)     DONE+=("$2"); shift 2 ;;
        --allow)    ALLOW+=("$2"); shift 2 ;;
        --timeout)  TIMEOUT="$2"; shift 2 ;;
        --need-led) NEED_LED="$2"; shift 2 ;;
        --need-flash) NEED_FLASH="$2"; shift 2 ;;
        --log)      LOG="$2"; shift 2 ;;
        --gdb)      GDB=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        -*)         echo "error: unknown option $1" >&2; usage; exit 2 ;;
        *)          ELF="$1"; shift ;;
    esac
done

[[ -n "${ELF:-}" ]] || { echo "error: no firmware ELF given" >&2; usage; exit 2; }
[[ -f "$ELF" ]] || { echo "error: firmware not found: $ELF" >&2; exit 2; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "error: qemu-system-arm not found (set QEMU=...)" >&2; exit 2; }

# The tests end in a busy blink loop; a done marker closes the run a moment later
# so the harness still sees a couple of LED toggles.
if [[ ${#DONE[@]} -eq 0 ]]; then
    DONE=("battery: done" "overlay: done" "RESULT:" "ALL OK" "scl-fw-xip")
fi

LOG="${LOG:-$(mktemp /tmp/qemu-run-XXXXXX.log)}"
echo "== qemu-run: $ELF"
echo "== machine:  $MACHINE"
[[ ${#ARGS[@]} -gt 0 ]] && echo "== semihosting cmdline: ${ARGS[*]}"
echo "== log:      $LOG"

# High-confidence fatal markers (never printed on a passing run).
FAIL_FAST=("HARDFAULT" "HardFault" "!! FAULT" "(FAILURES) ==" "SOME FAILED" "-> FAILURE" "FAIL — LED")
# Generic failure tokens, checked only after the run ends (a passing run never
# prints FAIL/FAILED; error-contract tests print PASS when the error occurred).
FAIL_TOKENS=("FAIL" "FAILED")

QEMU_CMD=( "$QEMU" -M "$MACHINE" -display none -monitor none -serial none )
# QEMU 11: one -semihosting-config carries the whole semihosting setup; arg=
# entries are comma-joined. (Repeating -semihosting-config overwrites and, in
# this QEMU build, garbles the command line.)
SHCFG="enable=on,target=native"
for a in "${ARGS[@]}"; do
    SHCFG+=",arg=${a}"
done
QEMU_CMD+=( -semihosting-config "$SHCFG" )
if [[ $GDB -eq 1 ]]; then
    # Interactive debugging: hand off to qemu-gdb.sh (qemu + arm-none-eabi-gdb).
    gdba=()
    for a in "${ARGS[@]}"; do gdba+=(--arg "$a"); done
    exec "$HERE/qemu-gdb.sh" --machine "$MACHINE" "${gdba[@]}" "$ELF"
fi
QEMU_CMD+=( -kernel "$ELF" )

# Start the emulator; guest console goes to the log.
"${QEMU_CMD[@]}" >"$LOG" 2>&1 &
QPID=$!

verdict="timeout"
done_seen=0
deadline=$((SECONDS + TIMEOUT))
while kill -0 "$QPID" 2>/dev/null; do
    for m in "${DONE[@]}"; do
        if grep -qF "$m" "$LOG" 2>/dev/null; then
            done_seen=1
            sleep 1                 # let the LED blink loop run a couple of toggles
            break 2
        fi
    done
    for m in "${FAIL_FAST[@]}"; do
        if grep -qF "$m" "$LOG" 2>/dev/null; then
            verdict="fail-fast"
            break 2
        fi
    done
    if (( SECONDS >= deadline )); then
        break
    fi
    sleep 0.2
done

if [[ $verdict == "timeout" ]]; then
    # The firmware may still be running; stop it.
    kill "$QPID" 2>/dev/null || true
fi
wait "$QPID" 2>/dev/null || true

# Final marker re-check: the emulator may have exited between poll iterations
# right after printing the completion marker (or on a lockup), so re-scan.
if [[ $done_seen -eq 0 ]]; then
    for m in "${DONE[@]}"; do
        if grep -qF "$m" "$LOG" 2>/dev/null; then done_seen=1; break; fi
    done
fi

# ---- verdict ----
led_count=$(grep -c '\[LED:' "$LOG" 2>/dev/null || true)
flash_files=$(grep -o 'flat_fs files *= *[0-9]*' "$LOG" 2>/dev/null | tail -1 | grep -o '[0-9]*$' || true)

fail=0
reason=()
if [[ $verdict == "fail-fast" ]]; then
    fail=1; reason+=("fatal marker: $(grep -m1 -E 'HARDFAULT|!! FAULT|FAILURES\) ==|SOME FAILED|-> FAILURE|FAIL — LED' "$LOG" || true)")
fi
if [[ $done_seen -eq 0 ]]; then
    # No completion marker: either a timeout or the emulator died early
    # (HardFault/Lockup). Either way the run did not complete -> FAIL.
    fail=1
    if (( SECONDS >= deadline )); then
        reason+=("timeout after ${TIMEOUT}s with no completion marker")
    else
        reason+=("firmware stopped before a completion marker (crash/lockup?)")
    fi
fi
for tok in "${FAIL_TOKENS[@]}"; do
    # Only a fail if no passing summary dominates it: error-contract tests
    # never print FAIL, so any FAIL token really is a failure -- unless the
    # line is an expected negative-path message (--allow).
    if grep -qF "$tok" "$LOG" 2>/dev/null; then
        if [[ $done_seen -eq 1 ]]; then
            if [[ ${#ALLOW[@]} -gt 0 ]]; then
                # Re-check the FAIL line while excluding allowed patterns.
                gv=(-vF)
                for a in "${ALLOW[@]}"; do gv+=(-e "$a"); done
                if ! grep -qF "$tok" <(grep "${gv[@]}" "$LOG" 2>/dev/null); then
                    break   # every FAIL was an expected one
                fi
            fi
            fail=1; reason+=("log contains '$tok'")
            break
        fi
    fi
done
if [[ $led_count -lt $NEED_LED ]]; then
    fail=1; reason+=("expected >= $NEED_LED LED toggles, saw $led_count")
fi
if [[ -n "$NEED_FLASH" && "$NEED_FLASH" != 0 && "${flash_files:-0}" -lt $NEED_FLASH ]]; then
    fail=1; reason+=("expected >= $NEED_FLASH flat_fs files, saw '${flash_files:-none}'")
fi

echo "== LED toggles:  $led_count"
[[ -n "$NEED_FLASH" && "$NEED_FLASH" != 0 ]] && echo "== flat_fs files: ${flash_files:-n/a}"
echo "------------------------------------------------------------"
# The semihosting console (guest output), including the LED on/off trace so
# the harness and user can see the LED actually toggling.
cat "$LOG"
if [[ $GDB -eq 1 && $done_seen -eq 1 ]]; then
    echo "== note: run with --gdb; qemu kept running (kill it with the gdb 'quit')"
    wait "$QPID" 2>/dev/null || true
fi
echo "------------------------------------------------------------"

if [[ $fail -eq 0 ]]; then
    echo "== QEMU TEST: PASS"
    exit 0
else
    echo "== QEMU TEST: FAIL"
    for r in "${reason[@]}"; do echo "   - $r"; done
    exit 1
fi

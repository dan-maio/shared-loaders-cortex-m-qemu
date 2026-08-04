#!/usr/bin/env bash
# qemu-run-with-tmp-fixtures.sh — stage a directory of .so fixtures into /tmp
# (the static-unify DYN suite dlopens hardcoded /tmp/*.so paths over
# semihosting), then boot + run the firmware under QEMU via qemu-run.sh.
#
# Usage: qemu-run-with-tmp-fixtures.sh <fixture-dir> [qemu-run.sh options...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DIR="${1:?usage: qemu-run-with-tmp-fixtures.sh <fixture-dir> [qemu-run options...]}"
shift
cp -f "$DIR"/*.so /tmp/
exec "$HERE/qemu-run.sh" "$@"

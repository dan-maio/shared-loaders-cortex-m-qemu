#!/usr/bin/env bash
# run-with-tmp-fixtures.sh — stage a directory of .so fixtures into /tmp (the
# static-unify DYN suite dlopens hardcoded /tmp/*.so paths over semihosting),
# then flash + run the firmware via flash-run.sh.
#
# Usage: run-with-tmp-fixtures.sh <fixture-dir> <app.hex|app-name>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DIR="${1:?usage: run-with-tmp-fixtures.sh <fixture-dir> <app.hex|app-name>}"
shift
cp -f "$DIR"/*.so /tmp/
exec "$HERE/flash-run.sh" "$@"

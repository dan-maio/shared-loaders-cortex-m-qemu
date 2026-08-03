#!/usr/bin/env bash
# flash-run-sf-make.sh — soft-float wrapper for flash-run-make.sh.
exec "$(dirname "$(readlink -f "$0")")/flash-run-make.sh" "$@"

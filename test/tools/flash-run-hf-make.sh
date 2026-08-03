#!/usr/bin/env bash
# flash-run-hf-make.sh — hard-float wrapper for flash-run-make.sh.
exec "$(dirname "$(readlink -f "$0")")/flash-run-make.sh" "$@"

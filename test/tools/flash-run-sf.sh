#!/usr/bin/env bash
# flash-run-sf.sh — flash a SOFT-float static app (or any app) via flash-run.sh.
# For the static apps this selects the build-dynamic-newlib tree + libs/build fixtures.
# (Freestanding apps are always hard-float; the variant is ignored for them.)
export SCL_DYNAMIC_NEWLIB_VARIANT=sf
exec "$(dirname "$(readlink -f "$0")")/flash-run.sh" "$@"

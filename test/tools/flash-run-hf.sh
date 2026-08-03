#!/usr/bin/env bash
# flash-run-hf.sh — flash a HARD-float static app (or any app) via flash-run.sh.
# For the static apps this selects the build-dynamic-newlib-hf tree + libs/build_hf
# fixtures (which include hf_test1/hf_test2). Build them with:
#   cmake -S test-dynamic/test-dynamic-newlib -B build-dynamic-newlib-hf \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake -DSCL_HF=ON
#   cmake --build build-dynamic-newlib-hf -j
export SCL_DYNAMIC_NEWLIB_VARIANT=hf
exec "$(dirname "$(readlink -f "$0")")/flash-run.sh" "$@"

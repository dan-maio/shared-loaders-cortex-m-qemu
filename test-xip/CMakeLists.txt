# XIP target test applications — mirrors test/ exactly, but each testN links the
# scl::xip-* loaders (R-X in flash, RW in RAM) instead of the copy-whole loaders.
# Configure with the ARM toolchain, same as test/:
#
#   cmake -S test-xip -B test-xip/build-fw -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
#   cmake --build test-xip/build-fw
#   ctest --test-dir test-xip/build-fw -R scl-fw-xip-nolib1   # flash + run (board needed)
#
# Heavy shared assets (platforms/, cmake/ helpers, common/) are REUSED from
# ../test rather than duplicated — see the README in each mirrored dir.
cmake_minimum_required(VERSION 3.20)
project(scl-xip-tests LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-xip/ builds Cortex-M4 firmware apps: configure with "
                        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)

enable_testing()

# Reuse the sibling test tree's app helper + platform sources verbatim.
set(SCL_TEST ${SCL_ROOT}/test)
include(${SCL_TEST}/cmake/scl-test-app.cmake)

add_subdirectory(test-xip-dynamic-nolib)
add_subdirectory(test-xip-dynamic-newlib)
add_subdirectory(test-xip-static)
add_subdirectory(test-xip-static-direct)
add_subdirectory(test-xip-interrupt)
add_subdirectory(test-xip-interrupt-static)
add_subdirectory(test-xip-interrupt-static-direct)
# test-xip-static/spi-flash builds standalone (like test-drv) because it
# removes -ffreestanding for scl::static. See its own build-and-run.sh.

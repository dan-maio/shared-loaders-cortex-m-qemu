# test/ — shared test infrastructure only. The firmware apps live in the
# per-technique trees: test-static/, test-dynamic/, test-xip/, test-overlay/,
# test-relocatable/ (see doc/test-taxonomy.md).
#
# Historic note: each testN is a Cortex-M4 FIRMWARE APP
# (main + SCL libraries), flashed and run on a WEACT-F411CE via OpenOCD.
# Configure this directory with the ARM toolchain:
#
#   cmake -S test -B test/build-fw -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
#   cmake --build test/build-fw            # builds every test app (.elf/.hex/.bin)
#   ctest --test-dir test/build-fw -R scl-fw-dyn1   # flash + run one app (board needed)
#
# Each testN/CMakeLists.txt can also be configured standalone the same way.
cmake_minimum_required(VERSION 3.20)
project(scl-tests LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test/ builds Cortex-M4 firmware apps: configure with "
                        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)

enable_testing()
include(cmake/scl-test-app.cmake)


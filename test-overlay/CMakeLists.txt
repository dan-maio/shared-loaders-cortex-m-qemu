# test-overlay — the overlay loading technique (scl::overlay).
#
# Modules are bound at link time to the SAME run address and stored back-to-back
# in flash; "loading" is a memcpy LMA -> VMA plus DSB/ISB. Only one module is
# resident in a window at a time.
#
#   cmake -S test-overlay -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
#   cmake --build build
#
# test1 also configures standalone the same way.
cmake_minimum_required(VERSION 3.20)
project(scl-tests-overlay LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-overlay/ builds Cortex-M4 firmware apps: configure with "
                        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)

enable_testing()
include(${SCL_ROOT}/test/cmake/scl-test-app.cmake)

add_subdirectory(test1)   # overlay-vs-dynamic comparison battery (shared SRAM window)

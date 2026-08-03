# test-dynamic — the dlopen/dlsym loading technique.
#
# scl::dynamic-nolib (freestanding, no heap) and scl::dynamic-newlib (hosted,
# real heap). Each app is Cortex-M4 firmware, flashed and run via OpenOCD:
#
#   cmake -S test-dynamic -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
#   cmake --build build
#
# Each subdirectory also configures standalone the same way.
cmake_minimum_required(VERSION 3.20)
project(scl-tests-dynamic LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-dynamic/ builds Cortex-M4 firmware apps: configure with "
                        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)

enable_testing()
include(${SCL_ROOT}/test/cmake/scl-test-app.cmake)

add_subdirectory(test-dynamic-newlib)
add_subdirectory(test-dynamic-nolib)
add_subdirectory(test-flatfs-bitbang-spi)
add_subdirectory(test-flatfs-spi)

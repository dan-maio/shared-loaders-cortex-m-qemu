# Static-loader firmware test apps. These need the HOSTED environment (newlib +
# CMSIS/HAL, vendored under the hosted platform) and therefore the hosted
# toolchain — they self-skip when the tree is configured with the freestanding
# toolchain (test/build-fw). Configure this directory standalone:
#
#   cmake -S test-static/test-static -B test/build-static \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake [-DSCL_HF=ON]
#   cmake --build test/build-static -j
#   ctest --test-dir test/build-static          # flash + run (board required)
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of the static test apps
    project(scl-static-tests LANGUAGES C CXX)
    set(SCL_HOSTED ON CACHE BOOL "" FORCE)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

add_subdirectory(test1)   # SCL battery: load/dlsym/call, journal-revert reload, error paths
add_subdirectory(test2)   # full original static-unify DYN suite (gotplt/concurrent/cross/...)

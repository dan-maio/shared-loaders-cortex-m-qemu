# test-static/test1 — hosted static-loader firmware app. Configure STANDALONE
# with the HOSTED toolchain (this app cannot live in the freestanding build-fw
# tree — different global flags):
#
#   cmake -S test-static/test-static -B test/build-static \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake
#   cmake --build test/build-static -j
#   ctest --test-dir test/build-static        # flash + run (board required)
#
# Prerequisite: the fixture .so files (vendored under the hosted platform) —
# built automatically via the scl-dynamic-newlib1-libs target (make -C libs/c).
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-dynamic-newlib1 LANGUAGES C CXX)
    set(SCL_HOSTED ON CACHE BOOL "" FORCE)   # scl::dynamic-newlib needs the hosted parts
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

if(NOT SCL_HOSTED OR NOT TARGET scl-dynamic-newlib)
    message(STATUS "test-static/test1 skipped: needs the hosted toolchain "
                   "(arm-none-eabi-hosted.cmake) and SCL_HOSTED=ON")
    return()
endif()

set(SCL_PLATFORM "board" CACHE STRING
    "Target platform (board for nucleo/weact, or qemu-f411re for qemu-system-arm)")
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../test/platforms/${SCL_PLATFORM}-hosted/cmake/platform-library.cmake)

add_executable(scl-fw-dynamic-newlib1
    src/main.cpp
    ${STATIC_UNIFY_DIR}/firmware/src/dl-impl.cpp   # the dl:: facade (vendored)
)
target_include_directories(scl-fw-dynamic-newlib1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(scl-fw-dynamic-newlib1 PRIVATE platform::${SCL_PLATFORM}-hosted scl::dynamic-newlib)
target_compile_definitions(scl-fw-dynamic-newlib1 PRIVATE DYN
    SCL_SO_DIR="${STATIC_UNIFY_DIR}/libs/build$<$<BOOL:${SCL_HF}>:_hf>"
    FIRMWARE_ELF_PATH="$<TARGET_FILE:scl-fw-dynamic-newlib1>")
# Keep-alive anchors: mylib1.so imports these from the firmware and the loader
# resolves them out of main's .symtab. main itself never calls them (it uses
# trace_printf), so without -u they are never pulled out of newlib/libm and the
# library's PLT/GOT slots stay null -- its constructors then branch to 0.
foreach(_a printf puts strcpy strcat strlen logf)
    list(APPEND _newlib1_uflags "LINKER:-u,${_a}")
endforeach()
target_link_options(scl-fw-dynamic-newlib1 PRIVATE
    ${_newlib1_uflags}
    LINKER:-Map=$<TARGET_FILE:scl-fw-dynamic-newlib1>.map)

scl_embed_symtab(scl-fw-dynamic-newlib1)   # must precede the .hex/.bin conversion

add_custom_command(TARGET scl-fw-dynamic-newlib1 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-dynamic-newlib1> $<TARGET_FILE:scl-fw-dynamic-newlib1>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-dynamic-newlib1> $<TARGET_FILE:scl-fw-dynamic-newlib1>.bin)

# Fixture .so files, built by the reference app's own sub-make.
add_custom_target(scl-dynamic-newlib1-libs
    COMMAND make -C ${STATIC_UNIFY_DIR}/libs/c all HF=$<BOOL:${SCL_HF}>
    COMMENT "Building vendored static fixture libraries (.so)")
add_dependencies(scl-fw-dynamic-newlib1 scl-dynamic-newlib1-libs)

# Run: execute the firmware under QEMU (qemu-f411re-hosted) with semihosting,
# or flash it via OpenOCD (board-hosted).
string(TOUPPER "${SCL_PLATFORM}" _SCL_PLATFORM_UPPER)
string(REPLACE "-" "_" _SCL_PLATFORM_VAR "${_SCL_PLATFORM_UPPER}")
set(_runner "${PLATFORM_${_SCL_PLATFORM_VAR}_HOSTED_RUNNER}")
if(_runner)
    add_test(NAME scl-fw-dynamic-newlib1
             COMMAND ${_runner} --machine ${PLATFORM_${_SCL_PLATFORM_VAR}_HOSTED_QEMU_MACHINE}
                     --done "RESULT:" $<TARGET_FILE:scl-fw-dynamic-newlib1>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
else()
    # Not linked -pie, but flash the .hex anyway for uniformity with the other apps.
    add_test(NAME scl-fw-dynamic-newlib1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../../test/tools/flash-run.sh
                     $<TARGET_FILE:scl-fw-dynamic-newlib1>.hex
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()

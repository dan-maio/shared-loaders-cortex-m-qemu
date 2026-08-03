# test-static/test2 — the FULL original static-unify DYN test suite as an SCL
# test app: gotplt walkthrough, concurrent libraries, coverage libs, mylib1/2,
# cross-library calls, libcppsh, c_test1/2 (+cross), C++ tests, complex
# scenario (+ hf_test1/2 with -DSCL_HF=ON). The app sources (main.cpp textually
# including tests-dyn.cpp, and the dl:: facade) are referenced IN PLACE from
# the vendored suite. Its dlopen paths are hardcoded to /tmp/*.so, so the ctest
# entry stages the fixture .so files into /tmp before flashing.
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-dynamic-newlib2 LANGUAGES C CXX)
    set(SCL_HOSTED ON CACHE BOOL "" FORCE)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

if(NOT SCL_HOSTED OR NOT TARGET scl-dynamic-newlib)
    message(STATUS "test-static/test2 skipped: needs the hosted toolchain "
                   "(arm-none-eabi-hosted.cmake) and SCL_HOSTED=ON")
    return()
endif()

set(SCL_PLATFORM "board-hosted" CACHE STRING "Unified hosted platform (do not change; board is SCL_BOARD)")
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../test/platforms/board-hosted/cmake/platform-library.cmake)

add_executable(scl-fw-dynamic-newlib2
    ${STATIC_UNIFY_DIR}/firmware/src/main.cpp      # DYN battery (includes tests-dyn.cpp)
    ${STATIC_UNIFY_DIR}/firmware/src/dl-impl.cpp   # the dl:: facade
)
target_include_directories(scl-fw-dynamic-newlib2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${STATIC_UNIFY_DIR}/firmware/src)              # tests-dyn.cpp textual include
target_link_libraries(scl-fw-dynamic-newlib2 PRIVATE platform::board-hosted scl::dynamic-newlib)
target_compile_definitions(scl-fw-dynamic-newlib2 PRIVATE DYN
    FIRMWARE_ELF_PATH="$<TARGET_FILE:scl-fw-dynamic-newlib2>")
# Keep-alive anchors: the union of what every fixture .so imports from main
# (see test1's CMakeLists for why -u is required).
foreach(_a printf puts snprintf fprintf _impure_ptr strcpy strcat strlen
           logf sinf cosf sqrtf __cxa_pure_virtual _ZnwjRKSt9nothrow_t)
    list(APPEND _newlib2_uflags "LINKER:-u,${_a}")
endforeach()
target_link_options(scl-fw-dynamic-newlib2 PRIVATE
    ${_newlib2_uflags}
    LINKER:-Map=$<TARGET_FILE:scl-fw-dynamic-newlib2>.map)

scl_embed_symtab(scl-fw-dynamic-newlib2)   # must precede the .hex/.bin conversion

add_custom_command(TARGET scl-fw-dynamic-newlib2 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-dynamic-newlib2> $<TARGET_FILE:scl-fw-dynamic-newlib2>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-dynamic-newlib2> $<TARGET_FILE:scl-fw-dynamic-newlib2>.bin)

# Fixture .so files — static2's tests-dyn.cpp dlopens ALL of these from /tmp:
#   libs/c            -> mylib1/2, cov1/2, complex_test, gotplt_test
#   libs/cpp          -> libcppsh
#   debug/test_libs   -> c_test1/2, cpp_test1/2
#   debug/test_hardfp -> hf_test1/2  (HARD float only; always outputs to build_hf)
# All go to vendor/libs/build (SF) or build_hf (HF). Missing any of these leaves
# a stale-ABI .so in /tmp and the flash-run ABI guard aborts, so build them ALL.
if(NOT TARGET scl-dynamic-newlib1-libs)   # also defined by test1
    add_custom_target(scl-dynamic-newlib1-libs
        COMMAND make -C ${STATIC_UNIFY_DIR}/libs/c all HF=$<BOOL:${SCL_HF}>
        COMMENT "Building vendored static fixture libraries: libs/c (.so)")
endif()
add_custom_target(scl-dynamic-newlib2-libs
    COMMAND make -C ${STATIC_UNIFY_DIR}/libs/cpp all HF=$<BOOL:${SCL_HF}>
    COMMAND make -C ${STATIC_UNIFY_DIR}/debug/test_libs all HF=$<BOOL:${SCL_HF}>
    DEPENDS scl-dynamic-newlib1-libs
    COMMENT "Building vendored static fixture libraries: libs/cpp + debug/test_libs (.so)")
# hf_test1/2 exist only for the hard-float suite; build them only when SCL_HF=ON.
if(SCL_HF)
    add_custom_target(scl-dynamic-newlib-hardfp
        COMMAND make -C ${STATIC_UNIFY_DIR}/debug/test_hardfp all
        DEPENDS scl-dynamic-newlib2-libs
        COMMENT "Building vendored hard-float fixtures: debug/test_hardfp (hf_test1/2)")
    add_dependencies(scl-fw-dynamic-newlib2 scl-dynamic-newlib-hardfp)
else()
    add_dependencies(scl-fw-dynamic-newlib2 scl-dynamic-newlib2-libs)
endif()

# On-target run: stage the .so fixtures into /tmp (the suite's hardcoded
# dlopen paths), then flash + semihosting console.
add_test(NAME scl-fw-dynamic-newlib2
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../../test/tools/run-with-tmp-fixtures.sh
                 ${STATIC_UNIFY_DIR}/libs/build$<$<BOOL:${SCL_HF}>:_hf>
                 $<TARGET_FILE:scl-fw-dynamic-newlib2>.hex
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

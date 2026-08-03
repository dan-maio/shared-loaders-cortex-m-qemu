cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-ovl1 LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../test/cmake/scl-test-app.cmake)
endif()

# Overlays-vs-dynamic comparison. The overlay modules are NON-PIC + -mlong-calls
# (they run in the SRAM window but call flash across >Thumb-BL reach; -fno-pic
# overrides the toolchain's global -fpic).
set_source_files_properties(src/ov1.c src/ov2.c
    PROPERTIES COMPILE_OPTIONS "-fno-pic;-mlong-calls")
set_source_files_properties(src/ovcpp.cpp
    PROPERTIES COMPILE_OPTIONS
    "-fno-pic;-mlong-calls;-fno-exceptions;-fno-rtti;-fno-use-cxa-atexit;-fno-unwind-tables")
scl_test_app(scl-fw-ovl1
    LIBS scl::dynamic-nolib scl::overlay
    LDSCRIPT sections-overlay.ld
    ANCHORS printf puts sinf cosf logf sqrtf
            trace_event trace_u32 fm_callback fm_var fm_info
    LINK_LIBM)

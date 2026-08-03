cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-flatfs LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../../test/cmake/scl-test-app.cmake)
endif()

# Flat-fs battery: provision the on-chip archive (sectors 6..7) from DL_LIB_DIR
# over semihosting on first boot, then run the 16-test battery loading every
# library FROM FLASH (dlopen_fs -> Flashbyte_source). Self-contained: the
# storage backend lives in this app's src/ + include/.
scl_test_app(scl-fw-flatfs
    LIBS scl::dynamic-nolib
    LDSCRIPT sections-flatfs.ld
    ANCHORS printf puts sinf cosf logf sqrtf
            trace_event trace_u32 fm_callback fm_var fm_info
    LINK_LIBM)

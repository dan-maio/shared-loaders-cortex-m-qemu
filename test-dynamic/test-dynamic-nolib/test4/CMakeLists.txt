cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    project(scl-fw-gotplt-demo LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../../test/cmake/scl-test-app.cmake)
endif()

scl_test_app(scl-fw-gotplt-demo
    LIBS scl::dynamic-nolib
    ANCHORS sinf cosf logf sqrtf trace_event trace_u32 fm_callback fm_var fm_info
            printf puts
    LINK_LIBM)

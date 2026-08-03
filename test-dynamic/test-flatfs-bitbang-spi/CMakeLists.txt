cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    project(scl-fw-flatfs-bitbang-spi LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../test/cmake/scl-test-app.cmake)
endif()

get_filename_component(_test3 ${CMAKE_CURRENT_SOURCE_DIR}/../test-dynamic-nolib/test3 ABSOLUTE)

scl_test_app(scl-fw-flatfs-bitbang-spi
    LIBS scl::dynamic-nolib
    LDSCRIPT sections.ld
    SOURCES ${_test3}/src/main-tests.cpp
    INCLUDE_DIRS ${_test3}/include
    ANCHORS printf puts sinf cosf logf sqrtf
            trace_event trace_u32 fm_callback fm_var fm_info
    LINK_LIBM)

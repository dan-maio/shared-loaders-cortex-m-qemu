cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-dyn1 LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../../test/cmake/scl-test-app.cmake)
endif()

# Demo: dlopen/dlsym/call/dlclose mylib1/2, libcppsh (C++), libcppeh (EH).
scl_test_app(scl-fw-dyn1
    LIBS scl::dynamic-nolib
    ANCHORS printf puts)

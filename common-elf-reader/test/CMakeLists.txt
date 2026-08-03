# common-elf-reader test — host-side verification of the ELF32 reader/parser.
#
# Builds a standalone test binary that exercises parse/headers/sections/segments
# on the 411ce-100Mhz.elf fixture. The reader is consumed as an INTERFACE
# source-code library (add_subdirectory + target_link_libraries).
#
#   cmake -B build && cmake --build build && ./build/elf-reader-test

cmake_minimum_required(VERSION 3.16)
project(elf-reader-test LANGUAGES CXX)

set(READER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(NOT EXISTS "${READER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "common-elf-reader not found at ${READER_DIR}")
endif()

# SCL_HOSTED=ON enables std::vector helpers (collect_relocations).
set(SCL_HOSTED ON CACHE BOOL "" FORCE)
add_subdirectory(${READER_DIR} common-elf-reader-build)

add_executable(elf-reader-test src/test-main.cpp)
target_link_libraries(elf-reader-test PRIVATE common-elf-reader::elf-reader)
target_compile_features(elf-reader-test PRIVATE cxx_std_20)

# Copy the .elf fixture next to the binary so the test can open it by name.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/411ce-100Mhz.elf
    ${CMAKE_CURRENT_BINARY_DIR}/411ce-100Mhz.elf
    COPYONLY
)

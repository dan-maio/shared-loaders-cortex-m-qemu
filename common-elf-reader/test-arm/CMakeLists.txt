# common-elf-reader test-arm — WeAct STM32F411CE @ 100 MHz, bare-metal.
#
# Full platform: startup (vector table + Reset_Handler), clock config
# (HSE 25 MHz → PLL → 100 MHz), semihosting I/O (BKPT 0xAB), libc shim.
# The reader is consumed as an INTERFACE source-code library.
#
# Build:
#   ./build.sh
#
# Flash + run:
#   ./run-test.sh

cmake_minimum_required(VERSION 3.16)
project(elf-reader-test-arm LANGUAGES C CXX)

set(READER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(NOT EXISTS "${READER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "common-elf-reader not found at ${READER_DIR}")
endif()

# Platform: WeAct STM32F411CE startup, board, semihosting, libc shim.
include(cmake/platform-library.cmake)

# SCL_HOSTED=OFF: freestanding, no std::vector helpers.
set(SCL_HOSTED OFF CACHE BOOL "" FORCE)
add_subdirectory(${READER_DIR} common-elf-reader-build)

add_executable(elf-reader-test-arm src/test-main.cpp)
target_link_libraries(elf-reader-test-arm PRIVATE
    common-elf-reader::elf-reader
    platform::test-arm)
target_compile_features(elf-reader-test-arm PRIVATE cxx_std_20)

# Copy the stripped .elf fixture (25 KB, no debug) for semihosting open().
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/411ce-100Mhz-stripped.elf
    ${CMAKE_CURRENT_BINARY_DIR}/411ce-100Mhz.elf
    COPYONLY
)

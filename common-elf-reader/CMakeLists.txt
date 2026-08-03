# common-elf-reader — standalone ARM ELF32-LE reader/parser.
#
# Exposes an INTERFACE "source code library" (no .a archive): its source
# propagates to each consuming target, which compiles it itself with its own
# flags. Both consumers (cortexm-shared-loader, shared-static-rework) integrate
# it this way so the exact same parser is built by source with one API.
#
#   add_subdirectory(common-elf-reader)      # or FetchContent / submodule
#   target_link_libraries(my_loader PRIVATE common-elf-reader::elf-reader)
#
# Options:
#   SCL_HOSTED=ON   enable std::vector helpers (elf_image::collect_relocations)
#                   for host tools/tests. Leave OFF for freestanding firmware.

cmake_minimum_required(VERSION 3.16)
project(common_elf_reader LANGUAGES CXX)

option(SCL_HOSTED "Enable hosted (std::vector) helpers" OFF)

# INTERFACE, not STATIC: the parser is compiled from source into each consumer
# (like cortexm's scl-common), never linked as a prebuilt archive.
add_library(elf-reader INTERFACE)
add_library(common-elf-reader::elf-reader ALIAS elf-reader)

target_sources(elf-reader INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/elf-image.cpp
)

target_include_directories(elf-reader INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(elf-reader INTERFACE cxx_std_20)

if(SCL_HOSTED)
    target_compile_definitions(elf-reader INTERFACE SCL_HOSTED)
endif()

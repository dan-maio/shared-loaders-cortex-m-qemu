# Cortex-M Source Code Libraries (SCL) — reusable shared-library loader code.
# Produces library targets only; never firmware. Integrate with:
#   add_subdirectory(shared-libs-loaders)
#   target_link_libraries(app PRIVATE scl::dynamic-nolib)   # INTERFACE: app compiles the sources
#
# Layout: common code lives directly in include/ and src/; loader-specific code
# lives in the dynamic-newlib/, dynamic-nolib/, overlay/ subfolders of each.
cmake_minimum_required(VERSION 3.20)
project(cortex-m-scl LANGUAGES C CXX)

option(SCL_BUILD_TESTS "Enable CTest registration for the firmware suites" ${PROJECT_IS_TOP_LEVEL})

# The ELF32 reader/parser (elf_image, byte_source, elf-types, result<T>) now
# lives in the standalone common-elf-reader repo, shared with shared-static-rework
# so both loaders parse through one API. It is consumed as source (same INTERFACE
# "source code library" pattern as scl-common), not as a prebuilt archive.
set(COMMON_ELF_READER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader"
    CACHE PATH "Path to the common-elf-reader repo (git submodule on this branch)")
if(NOT EXISTS "${COMMON_ELF_READER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "common-elf-reader not found at ${COMMON_ELF_READER_DIR}. "
        "Clone it beside this repo or set -DCOMMON_ELF_READER_DIR=<path>.")
endif()

# The common core is portable C++20; the port layer (asm trampolines, arena over
# linker symbols, _DYNAMIC self-parsing) and the loaders are Cortex-M only.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)" OR CMAKE_CROSSCOMPILING)
    set(SCL_TARGET_ARM ON)
else()
    set(SCL_TARGET_ARM OFF)
endif()

# SCL_HOSTED: parts needing a hosted C++ library (std::vector): the relocator
# plan builder, elf_image::collect_relocations, and the scl-dynamic-newlib module.
# Freestanding firmware (-ffreestanding -nostdlib, like the dynamic/overlay test
# apps) must keep this OFF and relocate via the allocation-free reloc_applier.
if(SCL_TARGET_ARM)
    option(SCL_HOSTED "Build hosted-only parts (std::vector): relocator, scl-dynamic-newlib" OFF)
else()
    option(SCL_HOSTED "Build hosted-only parts (std::vector): relocator, scl-dynamic-newlib" ON)
endif()

# The shared reader/parser, as an INTERFACE source-code library (no .a). Pulled
# in before scl-common so it can link it; SCL_HOSTED is already decided above and
# the child inherits it (same cache variable name), keeping collect_relocations
# in step with this build.
add_subdirectory(${COMMON_ELF_READER_DIR} common-elf-reader)

# ---- scl-common: code shared by all loader flavours -------------------------
# INTERFACE "source code library" (xpack pattern, e.g. micro-os-plus diag-trace):
# the SCL never compiles to a .a archive — its sources propagate to each
# consuming target, which compiles them itself with its own flags. True to
# SCL = *Source Code* Libraries.
add_library(scl-common INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lazy-resolve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/region-allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/reloc-applier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/symbol-resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scl-log-put-hex.c
)
target_include_directories(scl-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# The parser's source + include dir (loader/ headers) arrive transitively,
# compiled from source into each consumer.
target_link_libraries(scl-common INTERFACE common-elf-reader::elf-reader)
target_compile_features(scl-common INTERFACE cxx_std_20)
if(SCL_HOSTED)
    target_sources(scl-common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/relocator.cpp)
    target_compile_definitions(scl-common INTERFACE SCL_HOSTED=1)
endif()

if(SCL_TARGET_ARM)
    # ---- scl-dynamic-nolib: dlopen_source/dlsym/dlclose over any byte_source -
    # The freestanding dlopen loader (no libc). Includes the Cortex-M port layer
    # it depends on (VFP asm -> hard-float only); the newlib static loader uses
    # none of it, keeping scl-common portable and float-ABI-neutral.
    add_library(scl-dynamic-nolib INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-dynamic-nolib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-nolib/dl.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port/pic-trampoline.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port/dl-cross-call.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port/lazy-resolve.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port/unwind-find-exidx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/image-memory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/link-context.cpp
    )
    target_include_directories(scl-dynamic-nolib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/dynamic-nolib)
    target_link_libraries(scl-dynamic-nolib INTERFACE scl-common)

    # ---- scl-dynamic-newlib: link-time binding + runtime GOT patching, revert -
    # The newlib-based dynamic loader. Uses std::vector internally (dynamic-linker),
    # so consuming apps must link against a hosted C++ runtime.
    add_library(scl-dynamic-newlib INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-dynamic-newlib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-newlib/dynamic-linker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-newlib/patch-journal.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-newlib/image-memory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic-newlib/link-context.cpp
    )
    target_include_directories(scl-dynamic-newlib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/dynamic-newlib)
    target_link_libraries(scl-dynamic-newlib INTERFACE scl-common)

    # ---- scl-static: standalone ELF32 static patcher/loader (ex shared-static- -
    # rework). Resolves + GOT-patches PIC .so images at runtime; its elf_reader
    # is a thin adapter over the shared common-elf-reader parser. Loader support
    # (logger) travels with it. The demo firmware app lives in test-static/test-static.
    add_library(scl-static INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-static INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/static/elf-reader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/static/shlib-loader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/static/logger.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/static/module.cpp
    )
    target_include_directories(scl-static INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/static
    )
    target_link_libraries(scl-static INTERFACE common-elf-reader::elf-reader)
endif()

# ---- scl-overlay: linker-overlay window manager -----------------------------
# Portable C (the DSB/ISB barrier compiles away off-target). The application
# owns the linker script; template in src/overlay/ldscripts/overlay-fragment.ld.
add_library(scl-overlay INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-overlay INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay/overlay.c)
target_include_directories(scl-overlay INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include/overlay)
target_link_libraries(scl-overlay INTERFACE scl-common)

# ---- scl-reloc: ELF32 relocatable (ET_REL) module loader --------------------
# For -mlong-calls -fno-pic -r modules (kernel-module style): no GOT/PLT; place
# SHF_ALLOC sections in a RAM arena, resolve .symtab, apply R_ARM_ABS32/TARGET1/
# REL32/THM_CALL, run .init_array. Portable C++ (compiles on host; the actual
# call-into-module runs on target). Companion to the ET_DYN loaders, not a
# replacement. Reuses loader::symbol_source + the shared parser types.
add_library(scl-reloc INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-reloc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/reloc/reloc-loader.cpp)
target_include_directories(scl-reloc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(scl-reloc INTERFACE scl-common)

# ---- scl-xip-*: Execute-In-Place loaders (prototype) ------------------------
# XIP leaves each module's R-X segment in flash (executed in place) and copies
# only the RW segment to RAM: a two-base model (text_base = flash, data_base /
# r10 = RAM). Shared core + thin per-loader adapters, mirroring the existing
# loader flavours. See doc/xip-flash-execution-analysis.md.
add_library(scl-xip-common INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-xip-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/segment-map.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/relocate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/compact.cpp
)
target_include_directories(scl-xip-common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(scl-xip-common INTERFACE scl-common)

if(SCL_TARGET_ARM)
    # nolib and static XIP adapters ride on their base loaders.
    add_library(scl-xip-dynamic-nolib INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-xip-dynamic-nolib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/dynamic-nolib/adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/dynamic-nolib/xip-cross-call.c)
    target_link_libraries(scl-xip-dynamic-nolib INTERFACE scl-xip-common scl-dynamic-nolib)

    add_library(scl-xip-static INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-xip-static INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/static/adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/xip-cross-call.c)
    target_link_libraries(scl-xip-static INTERFACE scl-xip-common scl-static)

    add_library(scl-xip-dynamic-newlib INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-xip-dynamic-newlib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/dynamic-newlib/adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/xip/dynamic-nolib/xip-cross-call.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port/pic-trampoline.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/port/unwind-find-exidx.c)
    target_link_libraries(scl-xip-dynamic-newlib INTERFACE scl-xip-common scl-dynamic-newlib)
endif()

# ---- namespaced aliases (xpack-style) ---------------------------------------
# Canonical consumption:  target_link_libraries(app PRIVATE scl::dynamic-nolib)
# The "::" form makes CMake validate the name at configure time instead of
# passing a typo through to the linker as a plain string.
add_library(scl::common ALIAS scl-common)
add_library(scl::overlay ALIAS scl-overlay)
add_library(scl::reloc ALIAS scl-reloc)
add_library(scl::xip-common ALIAS scl-xip-common)
if(TARGET scl-xip-dynamic-nolib)
    add_library(scl::xip-dynamic-nolib ALIAS scl-xip-dynamic-nolib)
endif()
if(TARGET scl-xip-dynamic-newlib)
    add_library(scl::xip-dynamic-newlib ALIAS scl-xip-dynamic-newlib)
endif()
if(TARGET scl-xip-static)
    add_library(scl::xip-static ALIAS scl-xip-static)
endif()
if(TARGET scl-dynamic-nolib)
    add_library(scl::dynamic-nolib ALIAS scl-dynamic-nolib)
endif()
if(TARGET scl-dynamic-newlib)
    add_library(scl::dynamic-newlib ALIAS scl-dynamic-newlib)
endif()
if(TARGET scl-static)
    add_library(scl::static ALIAS scl-static)
endif()

# ---- tests ------------------------------------------------------------------
# The test applications are Cortex-M4 firmware, run on hardware via OpenOCD, and
# are a separate cross build per test project, e.g.:
#   cmake -S test-xip/test-xip-static/spi-flash -B build \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
if(SCL_BUILD_TESTS AND NOT SCL_TARGET_ARM)
    enable_testing()
endif()

# Define host symbol table generator (compiled for the host machine)
set(SCL_HOST_GEN_SYMTAB "${CMAKE_BINARY_DIR}/tools/gen-symtab" CACHE INTERNAL "")

if(NOT TARGET gen-symtab-host)
    add_custom_command(
        OUTPUT "${SCL_HOST_GEN_SYMTAB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tools"
        COMMAND c++ -std=c++20
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-symtab.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader/src/elf-image.cpp"
                "-I${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader/include"
                -o "${SCL_HOST_GEN_SYMTAB}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-symtab.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader/src/elf-image.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/common-elf-reader/include/loader/elf-image.hpp"
        COMMENT "Building C++20 gen-symtab host tool"
        VERBATIM
    )
    add_custom_target(gen-symtab-host DEPENDS "${SCL_HOST_GEN_SYMTAB}")
endif()

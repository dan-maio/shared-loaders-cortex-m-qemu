# scl::reloc test tree — a SEPARATE, self-contained Cortex-M4 firmware project.
# Unlike the ET_DYN test apps (test/), this main is an ordinary NON-PIC firmware:
# no -pie, no -Wl,-E, no .dynsym/GOT/_DYNAMIC. It reuses the plain platform
# sources (startup/semihost/board/crt) but its OWN simple linker script
# (platform/reloc.ld) — see build-linking-guide for why that suffices.
#
#   cmake -S test-relocatable -B test-relocatable/build \
#         -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
#   cmake --build test-relocatable/build            # -> scl-fw-reloc.hex (+ fixtures)
#   test-relocatable/tools/flash_reloc_run.sh       # flash + run on the board
cmake_minimum_required(VERSION 3.20)
project(scl-reloc-tests LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-relocatable builds a Cortex-M4 firmware app: configure with "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)
enable_testing()

# Platform selection: board (nucleo/weact hardware) or qemu-f411re (emulated).
set(SCL_PLATFORM "board" CACHE STRING "Target platform (board for nucleo/weact, or qemu-f411re for QEMU)")
set(PLAT ${SCL_ROOT}/test/platforms/${SCL_PLATFORM})
set(PLAT_COMMON ${SCL_ROOT}/test/platforms/common)

# Board boot/init switch + HSE value.
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(BOARD_DEFS "")
elseif(SCL_PLATFORM STREQUAL "board")
    set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
    if(SCL_BOARD STREQUAL "nucleo")
        set(BOARD_DEFS SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
    elseif(SCL_BOARD STREQUAL "weact")
        set(BOARD_DEFS SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
    else()
        message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
    endif()
else()
    message(FATAL_ERROR "SCL_PLATFORM must be 'board' or 'qemu-f411re' (got '${SCL_PLATFORM}')")
endif()

add_executable(scl-fw-reloc
    src/main.cpp
    ${PLAT}/src/startup.c      # vectors + reset (copy .data, zero .bss, FPU, main)
    ${PLAT}/src/semihost.c     # trace_puts/putc + semihost_open/flen/read/close
    ${PLAT}/src/board.c        # board_init / SysTick
    ${PLAT_COMMON}/src/crt.cpp        # operator new/delete + malloc/free (static heap)
    ${PLAT}/src/libc-shim.c)   # memcpy/memset/... (freestanding, -nostdlib)

target_include_directories(scl-fw-reloc PRIVATE ${PLAT}/include)
target_compile_definitions(scl-fw-reloc PRIVATE ${BOARD_DEFS})
target_link_libraries(scl-fw-reloc PRIVATE scl::reloc)   # loader + parser; no dlopen

# Ordinary NON-PIC firmware: override the toolchain's -fpic; no -pie at link.
target_compile_options(scl-fw-reloc PRIVATE
    -fno-pic -fno-pie -Os -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

# libm (cpp3's sinf/cosf/logf/sqrtf) + libgcc (compiler helpers), by multilib path.
separate_arguments(_mcu UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_C_COMPILER} ${_mcu} -print-file-name=libm.a
                OUTPUT_VARIABLE _libm OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_C_COMPILER} ${_mcu} -print-file-name=libgcc.a
                OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)

target_link_options(scl-fw-reloc PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld
    -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments)
target_link_libraries(scl-fw-reloc PRIVATE ${_libm} ${_libgcc})

add_custom_command(TARGET scl-fw-reloc POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-reloc> $<TARGET_FILE:scl-fw-reloc>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-reloc> $<TARGET_FILE:scl-fw-reloc>.bin)

# ---- The classic test firmware updated for ET_REL loader --------------------
add_executable(scl-fw-classic-reloc
    src/classic/main.cpp
    ${PLAT}/src/startup.c
    ${PLAT}/src/semihost.c
    ${PLAT}/src/board.c
    ${PLAT_COMMON}/src/crt.cpp
    ${PLAT}/src/libc-shim.c)

target_include_directories(scl-fw-classic-reloc PRIVATE ${PLAT}/include)
target_compile_definitions(scl-fw-classic-reloc PRIVATE ${BOARD_DEFS})
target_link_libraries(scl-fw-classic-reloc PRIVATE scl::reloc)

target_compile_options(scl-fw-classic-reloc PRIVATE
    -fno-pic -fno-pie -Os -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

target_link_options(scl-fw-classic-reloc PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld
    -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments)
target_link_libraries(scl-fw-classic-reloc PRIVATE ${_mcu} ${_libm} ${_libgcc})

add_custom_command(TARGET scl-fw-classic-reloc POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-classic-reloc> $<TARGET_FILE:scl-fw-classic-reloc>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-classic-reloc> $<TARGET_FILE:scl-fw-classic-reloc>.bin)

# ---- The interrupt-enabled test firmware for ET_REL loader -----------------
add_executable(scl-fw-reloc-interrupt
    src/main-interrupt.cpp
    ${PLAT}/src/startup.c
    ${PLAT}/src/semihost.c
    ${PLAT}/src/board.c
    ${PLAT_COMMON}/src/crt.cpp
    ${PLAT_COMMON}/src/semihost-file.cpp
    ${PLAT}/src/libc-shim.c)

target_include_directories(scl-fw-reloc-interrupt PRIVATE 
    ${PLAT}/include
)
target_compile_definitions(scl-fw-reloc-interrupt PRIVATE ${BOARD_DEFS})
target_link_libraries(scl-fw-reloc-interrupt PRIVATE scl::reloc)

target_compile_options(scl-fw-reloc-interrupt PRIVATE
    -fno-pic -fno-pie -Os -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

target_link_options(scl-fw-reloc-interrupt PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld
    -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments)
target_link_libraries(scl-fw-reloc-interrupt PRIVATE ${_libm} ${_libgcc})

add_custom_command(TARGET scl-fw-reloc-interrupt POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-reloc-interrupt> $<TARGET_FILE:scl-fw-reloc-interrupt>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-reloc-interrupt> $<TARGET_FILE:scl-fw-reloc-interrupt>.bin)

# Build the ET_REL fixtures and point the app at them (opened over semihosting).
set(RELOC_FIX ${CMAKE_CURRENT_SOURCE_DIR}/fixtures-reloc)
get_filename_component(SCL_TCBIN ${CMAKE_C_COMPILER} DIRECTORY)
add_custom_target(reloc-fixtures
    COMMAND make -C ${RELOC_FIX} TOOLCHAIN_PATH=${SCL_TCBIN}
    COMMENT "Building ET_REL fixtures (relprov/relmod/c1/c2/cpp3 .rel.o)" VERBATIM)
add_dependencies(scl-fw-reloc reloc-fixtures)
target_compile_definitions(scl-fw-reloc PRIVATE RELOC_LIB_DIR="${RELOC_FIX}/build")

add_dependencies(scl-fw-classic-reloc reloc-fixtures)
target_compile_definitions(scl-fw-classic-reloc PRIVATE RELOC_LIB_DIR="${RELOC_FIX}/build")

add_dependencies(scl-fw-reloc-interrupt reloc-fixtures)
target_compile_definitions(scl-fw-reloc-interrupt PRIVATE RELOC_LIB_DIR="${RELOC_FIX}/build" FIRMWARE_ELF_PATH="$<TARGET_FILE:scl-fw-reloc-interrupt>")

# ---- classic-spi: classic test loading modules from SPI flash (W25Q64) -------
# SPI-flash app: WEACT-only (the NUCLEO-F411RE has no SPI wiring).
set(_su ${SCL_ROOT}/test/platforms/board-hosted/vendor)
add_executable(scl-fw-classic-spi-reloc
    src/classic-spi/main.cpp
    ${PLAT}/src/startup.c
    ${PLAT}/src/semihost.c
    ${PLAT}/src/board.c
    ${PLAT_COMMON}/src/crt.cpp
    ${PLAT}/src/libc-shim.c
    ${SCL_ROOT}/test-dynamic/test-flatfs-spi/src/w25q64.cpp
    ${SCL_ROOT}/test-dynamic/test-flatfs-spi/src/fs-archive-spi.cpp
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_spi.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_dma.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr_ex.c
    ${_su}/common/system/src/cmsis/system_stm32f4xx.c)

target_include_directories(scl-fw-classic-spi-reloc PRIVATE
    ${PLAT}/include
    ${SCL_ROOT}/test-dynamic/test-flatfs-spi/include
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include)

target_link_libraries(scl-fw-classic-spi-reloc PRIVATE scl::reloc)

target_compile_options(scl-fw-classic-spi-reloc PRIVATE
    -fno-pic -fno-pie -Os -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

target_compile_definitions(scl-fw-classic-spi-reloc PRIVATE
    ${BOARD_DEFS}
    STM32F411xE USE_HAL_DRIVER
    RELOC_LIB_DIR="${RELOC_FIX}/build")

target_link_options(scl-fw-classic-spi-reloc PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld
    -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments)
target_link_libraries(scl-fw-classic-spi-reloc PRIVATE ${_libm} ${_libgcc})

add_custom_command(TARGET scl-fw-classic-spi-reloc POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-classic-spi-reloc> $<TARGET_FILE:scl-fw-classic-spi-reloc>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-classic-spi-reloc> $<TARGET_FILE:scl-fw-classic-spi-reloc>.bin)

add_dependencies(scl-fw-classic-spi-reloc reloc-fixtures)

add_test(NAME scl-fw-classic-spi-reloc
         COMMAND ${SCL_ROOT}/test/tools/flash-run.sh $<TARGET_FILE:scl-fw-classic-spi-reloc>.hex
         WORKING_DIRECTORY ${SCL_ROOT})

# ---- flatfs: embed the fsmod1/fsmod2 .rel.o into FLASH (const .rodata arrays)
# and link them into the classic app, so its flatfs demo loads them from flash
# (no semihosting FS). embed-blob.py runs after the fixtures are built.
set(FLATFS_BLOBDIR ${CMAKE_CURRENT_BINARY_DIR}/blobs)
file(MAKE_DIRECTORY ${FLATFS_BLOBDIR})
foreach(m fsmod1 fsmod2)
    set(_blob ${FLATFS_BLOBDIR}/${m}_blob.c)
    add_custom_command(OUTPUT ${_blob}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-blob.py
                ${RELOC_FIX}/build/${m}.rel.o ${m} ${_blob}
        DEPENDS reloc-fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-blob.py
        COMMENT "Embedding ${m}.rel.o -> ${m}_blob.c (flatfs)" VERBATIM)
    target_sources(scl-fw-classic-reloc PRIVATE ${_blob})
endforeach()

# ---- real flatfs: pack fsmod1+fsmod2 into ONE flash archive (directory + data)
# via pack-flatfs.py, and link it in. The classic demo mounts it and loads
# modules BY NAME — the genuine "filesystem in flash" (vs the blobs above).
set(FLATFS_ARCHIVE ${FLATFS_BLOBDIR}/flatfs_archive.c)
add_custom_command(OUTPUT ${FLATFS_ARCHIVE}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack-flatfs.py
            ${FLATFS_ARCHIVE} flatfs_archive
            fsmod1=${RELOC_FIX}/build/fsmod1.rel.o
            fsmod2=${RELOC_FIX}/build/fsmod2.rel.o
    DEPENDS reloc-fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack-flatfs.py
    COMMENT "Packing fsmod1+fsmod2 -> flatfs_archive.c (real flatfs)" VERBATIM)
target_sources(scl-fw-classic-reloc PRIVATE ${FLATFS_ARCHIVE})

# On-target run: reuse the shared flash-run.sh (OpenOCD program + semihosting).
add_test(NAME scl-fw-reloc
         COMMAND ${SCL_ROOT}/test/tools/flash-run.sh $<TARGET_FILE:scl-fw-reloc>.hex
         WORKING_DIRECTORY ${SCL_ROOT})

add_test(NAME scl-fw-classic-reloc
         COMMAND ${SCL_ROOT}/test/tools/flash-run.sh $<TARGET_FILE:scl-fw-classic-reloc>.hex
         WORKING_DIRECTORY ${SCL_ROOT})

add_test(NAME scl-fw-reloc-interrupt
         COMMAND ${SCL_ROOT}/test/tools/flash-run.sh $<TARGET_FILE:scl-fw-reloc-interrupt>.hex
         WORKING_DIRECTORY ${SCL_ROOT})

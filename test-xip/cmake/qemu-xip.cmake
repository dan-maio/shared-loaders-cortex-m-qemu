# qemu-xip.cmake — shared QEMU wiring for the XIP test apps.
#
# The XIP CMakeLists define their own platform target from the vendored
# nucleo-f411re-hosted tree. Under QEMU (SCL_PLATFORM=qemu-f411re) three things
# must change, and this helper centralises them so each app's CMakeLists stays
# a one-line include:
#   * the clock init  -> qemu-f411re-hosted/src/initialize-hardware.c (no HSE/PLL
#     ready-bit polling; the emulator never sets them),
#   * the LED         -> qemu-f411re-hosted/src/led.cpp (also traces over
#     semihosting so the harness can verify it),
#   * the flat_fs linker script -> a QEMU variant whose FLASH_FS region is CCM
#     SRAM (the emulated flash is read-only), plus a constructor that pre-fills
#     it to the erased 0xFF state.
#
# Include AFTER the app has set `_repo` and `_su`:
#   include(${_repo}/test-xip/cmake/qemu-xip.cmake)
# The app must replace its initialize-hardware.c / led.cpp references with
# ${XIP_INIT} / ${XIP_LED}, add ${XIP_FS_FILL} to the platform sources, link
# with -T${XIP_FLATFS_LD}, and register the test with ${XIP_RUNNER}.

set(SCL_PLATFORM "board" CACHE STRING
    "Target platform (board for nucleo/weact, or qemu-f411re for qemu-system-arm)")

set(XIP_QEMU_INIT    ${_repo}/test/platforms/qemu-f411re-hosted/src/initialize-hardware.c)
set(XIP_QEMU_LED     ${_repo}/test/platforms/qemu-f411re-hosted/src/led.cpp)
set(XIP_QEMU_FS_FILL ${_repo}/test-xip/cmake/qemu-fs-fill.c)
set(XIP_QEMU_LD_NOLIB  ${_repo}/test-xip/cmake/sections-flatfs-qemu-nolib.ld)
set(XIP_QEMU_LD_NEWLIB ${_repo}/test-xip/cmake/sections-flatfs-qemu-newlib.ld)
set(XIP_QEMU_RUNNER  ${_repo}/test/tools/qemu-run.sh)
set(XIP_QEMU_MACHINE netduinoplus2)

if(SCL_PLATFORM MATCHES "^qemu-")
    set(XIP_INIT      ${XIP_QEMU_INIT})
    set(XIP_LED       ${XIP_QEMU_LED})
    set(XIP_FS_FILL   ${XIP_QEMU_FS_FILL})
    set(XIP_FLATFS_LD ${XIP_QEMU_LD_NOLIB})      # newlib apps override below
    set(XIP_RUNNER    ${XIP_QEMU_RUNNER})
    set(XIP_MACHINE   ${XIP_QEMU_MACHINE})
else()
    set(XIP_INIT      ${_su}/common/firmware/src/initialize-hardware.c)
    set(XIP_LED       ${_su}/common/firmware/src/led.cpp)
    set(XIP_FLATFS_LD ${CMAKE_CURRENT_SOURCE_DIR}/src/sections-flatfs-hosted.ld)
    set(XIP_FS_FILL   "")
endif()

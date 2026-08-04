/* STM32F411RE memory map -- OVERLAY build (static, link-time-bound overlays).
 *
 * Unlike the dynamic-loader builds (main / flat_fs), nothing here is loaded as an
 * ELF at runtime: the three overlay modules are linked INTO this single firmware
 * image, stored back-to-back in FLASH (their LMA), and copied on demand into ONE
 * shared SRAM window (their common VMA). Address resolution is entirely link-time.
 *
 * RAM is partitioned into three NON-OVERLAPPING regions so ld itself enforces the
 * layout (a region overflow is a hard link error, not a silent stomp):
 *
 *   RAM  0x20000000..0x20018000  (96K)  .data/.dynamic/.got/.bss + image arena
 *   OVL  0x20018000..0x2001C000  (16K)  the SHARED overlay window (one module at a time)
 *   ---- 0x2001C000..0x20020000  (16K)  stack (grows down from _estack)
 *
 * The image arena (used by the dynamic-loader half of the comparison: it dlopen()s
 * c1/c2/cpp3 to show runtime relocation side-by-side) lives in RAM, well clear of
 * the overlay window and the stack.
 */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
  OVL   (rwx) : ORIGIN = 0x20018000, LENGTH = 16K
}

/* Initial stack pointer = top of RAM. The stack occupies the 16K gap ABOVE the
 * overlay window (0x2001C000..0x20020000); it never overlaps OVL or RAM. */
_estack = 0x20020000;

/* Shared overlay window geometry (consumed by the overlay manager in main-overlay.cpp). */
_ovl_window      = ORIGIN(OVL);   /* 0x20018000: the common VMA every overlay runs at */
_ovl_window_size = LENGTH(OVL);   /* 0x00004000 (16K) */

/* RAM arena for the dynamic-loader comparison (used by image-memory.cpp). 32K holds
 * c1/c2/cpp3 with margin; the overlay half needs none of it. */
_simage_arena_size = 32K;

/* STM32F411RE memory map. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* Initial stack pointer = top of RAM (loaded by hardware from vector[0]). */
_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 0x20020000 */

/* Reserved RAM arena for 4b loaded library images (declared now, used in 4b). */
_simage_arena_size = 64K;

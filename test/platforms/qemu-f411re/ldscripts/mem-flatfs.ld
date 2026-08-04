/* qemu-f411re memory map — flat_fs (QEMU variant).
 *
 * The emulated STM32F405 flash is read-only ROM, so a hardware-style FLASH_FS
 * region at 0x08040000 cannot be written. Instead we back the "flash
 * filesystem" with the emulator's real CCM SRAM (0x10000000, 64K on this
 * machine): the firmware's flash driver writes words directly to the region
 * address, so the flat_fs archive is RAM-backed and behaves identically to NOR
 * flash for the loader (memory-mapped reads, 1->0 word programming). Erase
 * becomes a no-op (RAM is overwritable), which the driver tolerates.
 *
 * The rest of the map matches the hardware board (512K code flash, 128K RAM).
 */
MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  FLASH_FS (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* Initial stack pointer = top of RAM (loaded by hardware from vector[0]). */
_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 0x20020000 */

/* Filesystem region geometry. These are absolute (value) symbols consumed at
 * RUNTIME by fs-archive.cpp; ld does not place any output section here. */
_fs_base = ORIGIN(FLASH_FS);   /* 0x10000000 */
_fs_size = LENGTH(FLASH_FS);   /* 0x0000C000 (48K) */

/* Reserved RAM arena for loaded library images (used by image-memory.cpp). */
_simage_arena_size = 64K;

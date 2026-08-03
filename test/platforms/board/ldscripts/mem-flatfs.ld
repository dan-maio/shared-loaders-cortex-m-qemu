/* STM32F411RE memory map — flat_fs split (Option A: 256K code / 256K filesystem).
 *
 * The 512K flash is split ON A SECTOR BOUNDARY into two independent halves:
 *
 *   FLASH    (code) : sectors 0..5  = 4x16K + 64K + 128K = 256K  @ 0x08000000
 *   FLASH_FS (data) : sectors 6..7  = 128K + 128K        = 256K  @ 0x08040000
 *
 * 0x08040000 is the start of sector 6, so the filesystem region can be erased
 * independently of code. (A 320K boundary at 0x08050000 would fall INSIDE
 * sector 6 -- erase is per-sector on this single-bank part -- so it could not
 * be erased without destroying code. See docs/FLAT_FS_DESIGN.md.)
 *
 * Shrinking FLASH to 256K also makes ld fail the link if code ever spills past
 * 0x08040000, which would otherwise silently land in the filesystem region.
 */
MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  FLASH_FS (r)   : ORIGIN = 0x08040000, LENGTH = 256K
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* Initial stack pointer = top of RAM (loaded by hardware from vector[0]). */
_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 0x20020000 */

/* Filesystem region geometry. These are absolute (value) symbols consumed at
 * RUNTIME by fs-archive.cpp; ld does not place any output section here -- the
 * region is written by the firmware itself via the RAM-resident flash driver. */
_fs_base = ORIGIN(FLASH_FS);   /* 0x08040000 */
_fs_size = LENGTH(FLASH_FS);   /* 0x00040000 (256K) */

/* Reserved RAM arena for loaded library images (used by image-memory.cpp). */
_simage_arena_size = 64K;

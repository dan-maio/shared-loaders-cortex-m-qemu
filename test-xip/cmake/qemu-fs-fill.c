/* qemu-fs-fill.c — pre-fill the RAM-backed flat_fs region with 0xFF.
 *
 * The XIP apps back their flat_fs "flash filesystem" with CCM SRAM under QEMU
 * (see test-xip/cmake/sections-flatfs-qemu-*.ld): QEMU zeroes RAM, but the
 * append-log archive needs the erased-NOR 0xFF state (free TOC slots ==
 * 0xFFFFFFFF). This constructor runs before main() (newlib startup runs the
 * init array) and fills the region. Apps without a flat_fs region link
 * _fs_base/_fs_size as 0 (weak), making the loop a no-op.
 */
#include <stdint.h>

extern unsigned char _fs_base[] __attribute__((weak));
extern unsigned char _fs_size[] __attribute__((weak));

__attribute__((constructor))
static void qemu_fs_fill(void)
{
    unsigned char* p = _fs_base;
    unsigned char* e = p + (unsigned int)(uintptr_t)_fs_size;
    while (p < e) *p++ = 0xFFu;
}

/**
 * @file unwind-find-exidx.c
 * @brief Strong override of libgcc's __gnu_Unwind_Find_exidx for the bare-metal
 *        dynamic loader (Plan 5d).
 *
 * libgcc references __gnu_Unwind_Find_exidx as a WEAK UNDEFINED symbol: the ARM
 * EHABI unwinder calls it to find the .ARM.exidx table covering a faulting PC, and
 * expects the platform/BSP to define it. Under -nostdlib/-nostartfiles there is no
 * BSP, so without this definition the call would resolve to 0 and HardFault on the
 * first unwind. We define it strongly:
 *   - if pc is inside a loaded library's image -> return that library's *relocated*
 *     .ARM.exidx range (registered by dlopen);
 *   - otherwise -> fall back to main's own table (__exidx_start/__exidx_end).
 * This is the single hook the raise/catch path needs (no _Unwind_Backtrace API).
 */
#include "scl/unwind-exidx.h"

/* Matches libgcc's unwind-arm-common.h: pointer-sized unsigned. */
typedef unsigned _Unwind_Ptr __attribute__((__mode__(__pointer__)));

/** @brief One EHABI index-table entry (two words, 8 bytes). main's table is
 *         delimited by the linker symbols from firmware/ldscripts/sections.ld. */
typedef struct { unsigned w0, w1; } exidx_entry_t;
extern exidx_entry_t __exidx_start[];  /**< First entry of main's own .ARM.exidx. */
extern exidx_entry_t __exidx_end[];    /**< One past main's last .ARM.exidx entry. */

#define DL_EXIDX_MAX 4   /**< Max simultaneously-registered loaded-image exidx tables. */

/** @brief A loaded image's registered (relocated) .ARM.exidx range. */
typedef struct {
    uint32_t img_base;     /**< Image load bias (base address). */
    uint32_t img_size;     /**< Image span in bytes. */
    uint32_t exidx_base;   /**< Relocated .ARM.exidx start address. */
    int      exidx_count;  /**< Number of 8-byte EHABI entries. */
    int      used;         /**< Slot occupied flag. */
} exidx_reg_t;

static exidx_reg_t g_exidx[DL_EXIDX_MAX];  /**< Registry of loaded-image unwind tables. */

void dl_exidx_register(uint32_t img_base, uint32_t img_size,
                       uint32_t exidx_base, int exidx_count) {
    if (exidx_base == 0 || exidx_count <= 0) return;  /* no unwind table in this image */
    for (int i = 0; i < DL_EXIDX_MAX; ++i) {
        if (!g_exidx[i].used) {
            g_exidx[i].img_base    = img_base;
            g_exidx[i].img_size    = img_size;
            g_exidx[i].exidx_base  = exidx_base;
            g_exidx[i].exidx_count = exidx_count;
            g_exidx[i].used        = 1;
            return;
        }
    }
}

void dl_exidx_unregister(uint32_t img_base) {
    for (int i = 0; i < DL_EXIDX_MAX; ++i)
        if (g_exidx[i].used && g_exidx[i].img_base == img_base)
            g_exidx[i].used = 0;
}

/**
 * @brief libgcc EHABI hook: return the .ARM.exidx table covering @p pc.
 * @param pc      The faulting/unwinding program counter.
 * @param pcount  Out: number of 8-byte entries in the returned table.
 * @return Address of the matching exidx table (a loaded image's, or main's fallback).
 */
_Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *pcount) {
    const uint32_t a = (uint32_t)pc;
    for (int i = 0; i < DL_EXIDX_MAX; ++i) {
        if (g_exidx[i].used &&
            a >= g_exidx[i].img_base && a < g_exidx[i].img_base + g_exidx[i].img_size) {
            *pcount = g_exidx[i].exidx_count;
            return (_Unwind_Ptr)g_exidx[i].exidx_base;
        }
    }
    *pcount = (int)(__exidx_end - __exidx_start);   /* main's own frames */
    return (_Unwind_Ptr)__exidx_start;
}

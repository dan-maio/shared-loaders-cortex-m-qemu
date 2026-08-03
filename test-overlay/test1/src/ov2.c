/**
 * @file ov2.c
 * @brief Overlay module 2 (C) -- the static counterpart of c2.so's exported data
 *        (battery TEST 4: FM reads c2_data + c2_const). main reads ov2_data (.data)
 *        and ov2_const (.rodata) directly by symbol once this overlay is resident.
 *
 * Same build model as ov1.c (non-PIC, -mlong-calls, link-time-bound). The exported
 * symbols resolve, in main's -fpic GOT, to addresses INSIDE the shared overlay
 * window -- so they are only valid while ov2 is the loaded module. Load a different
 * overlay and those same addresses now hold that module's bytes (the whole point).
 */
#include <stdint.h>
#include "fm-iface.h"

#define OV2_TEXT   __attribute__((section(".ov2.text"),   noinline, used))  /**< Place code   in the ov2 overlay. */
#define OV2_DATA   __attribute__((section(".ov2.data"),   used))            /**< Place data   in the ov2 overlay. */
#define OV2_RODATA __attribute__((section(".ov2.rodata"), used))            /**< Place rodata in the ov2 overlay. */

OV2_DATA   uint32_t       ov2_data  = 0x2200u;   /**< Exported writable .data (== c2_data), in the window. */
OV2_RODATA const uint32_t ov2_const = 0x02C0u;   /**< Exported read-only .rodata (== c2_const), in the window. */

/** @brief Read this module's own exported data and trace via main; returns ov2_data + ov2_const (9408). */
OV2_TEXT uint32_t ov2_sum(void)
{
    trace_event("ov2", "ov2_sum", ov2_data);
    return ov2_data + ov2_const;                 /* 0x2200 + 0x02C0 = 0x24C0 (9408) */
}

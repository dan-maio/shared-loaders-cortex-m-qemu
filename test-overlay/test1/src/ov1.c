/**
 * @file ov1.c
 * @brief Overlay module 1 (C) -- the static counterpart of c1.so:c1_use_fm
 *        (battery TEST 5). It READS main's exported var (fm_var, RAM .data) and
 *        const aggregate (fm_info, FLASH .rodata) and CALLS main's trace_* helpers.
 *
 * Built NON-PIC with -mlong-calls and placed by the linker in the shared overlay
 * window: every reference to main is bound to a FIXED address at LINK TIME. The
 * trace and fm_ targets live ~400MB away in FLASH/lower-RAM (far beyond a Thumb BL),
 * so -mlong-calls turns each call into a register-indirect BLX (unlimited range);
 * data reads use MOVW/MOVT absolute addressing. Nothing is relocated at runtime --
 * the overlay manager only memcpy()s these bytes into the window before the call.
 *
 * Unlike c1.so (whose c1_obj.magic is stamped by a dlopen-time constructor), an
 * overlay runs no constructors: ov1_obj is pre-initialised in .data and that image
 * is copied into the window with the code. Same end value, different mechanism.
 */
#include <stdint.h>
#include "fm-iface.h"   /* main's exported surface: trace_event/trace_u32 + fm_var/fm_info */

#define OV1_TEXT   __attribute__((section(".ov1.text"),   noinline, used))  /**< Place code   in the ov1 overlay. */
#define OV1_DATA   __attribute__((section(".ov1.data"),   used))            /**< Place data   in the ov1 overlay. */
#define OV1_RODATA __attribute__((section(".ov1.rodata"), used))            /**< Place rodata in the ov1 overlay. */

/** @brief "static class": a file-scope struct, pre-initialised in the overlay's .data. */
struct Ov1Obj {
    uint32_t magic;   /**< Identity stamp (0x00C1 == 193); preset, no constructor runs. */
    uint32_t count;   /**< Bumped on each ov1_use_fm() call (proves .data is in the window). */
};
OV1_DATA   struct Ov1Obj ov1_obj = { 0x00C1u, 1u };   /**< The overlay's object state (magic 193). */
OV1_DATA   uint32_t      ov1_data = 0x1100u;          /**< Exported .data sample. */
OV1_RODATA const char    ov1_tag[] = "ov1: hello";    /**< Exported .rodata string. */

/**
 * @brief Mirror of c1.so:c1_use_fm. Reads main's fm_var/fm_info and calls main's trace_*.
 * @return fm_var(170) + fm_info.id(241) + fm_info.flags(7) + ov1_obj.magic(193) = 611.
 */
OV1_TEXT uint32_t ov1_use_fm(void)
{
    trace_event("ov1", "ov1_use_fm(fm_var)", fm_var);   /* -> main (FLASH), long_call */
    trace_u32("ov1: fm_info.id = ", fm_info.id);        /* reads main .rodata aggregate */
    ov1_obj.count += 1;
    return fm_var + fm_info.id + fm_info.flags + ov1_obj.magic;
}

/** @brief Current value of ov1_obj.count (the in-window call counter). */
OV1_TEXT uint32_t ov1_count(void) { return ov1_obj.count; }

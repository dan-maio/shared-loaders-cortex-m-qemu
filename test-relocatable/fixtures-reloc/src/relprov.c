/**
 * @file relprov.c
 * @brief relprov — a tiny ET_REL *provider* module for the scl::reloc loader.
 *
 * Loaded first; @ref relmod imports prov_add() from it. Because everything is
 * built -mlong-calls -fno-pic, that cross-module call is a plain absolute call
 * (an R_ARM_ABS32 literal the loader points at relprov's runtime address) — NO
 * GOT, NO PLT, NO r10/trampoline. Contrast the ET_DYN loaders, where a cross-lib
 * call is a JUMP_SLOT routed through dl_cross_call with an r10 swap.
 *
 * Relocations this module carries: R_ARM_ABS32 (the trace_event pointer + the
 * prov_data references) and a .init_array entry (the constructor).
 */
#include <stdint.h>
#include "fm-iface.h"   /* main's exported trace ABI */

uint32_t        prov_data  = 0x2200;   /**< .data, exported (ctor bumps it). */
static uint32_t prov_calls;            /**< .bss  (proves zero-init). */

/** @brief Constructor -> .init_array. Proves the loader ran ctors and that
 *         .data/.bss were placed (stamps prov_data, which starts in .data). */
__attribute__((constructor))
static void prov_ctor(void) {
    prov_data += 0x11;                  /* 0x2200 -> 0x2211 */
    trace_event("prov", "ctor", prov_data);
}

/** @brief Exported: relmod calls this across modules (absolute, no r10). */
uint32_t prov_add(uint32_t a, uint32_t b) {
    prov_calls++;
    return a + b + prov_data;
}

/** @brief Exported: how many times prov_add ran (reads its own .bss). */
uint32_t prov_state(void) { return prov_calls; }

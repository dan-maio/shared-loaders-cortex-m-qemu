/**
 * @file relmod.c
 * @brief relmod — the consumer ET_REL module for the scl::reloc loader.
 *
 * Exercises every relocation the loader implements, with NO GOT/PLT/r10 anywhere
 * (that machinery is an ET_DYN concept; these modules are -mlong-calls -fno-pic):
 *
 *   - R_ARM_ABS32   : the rel_data reference, the "reloc: hello" string literal,
 *                     and the function-pointer literals to main (trace_event,
 *                     fm_callback) and to relprov (prov_add).
 *   - R_ARM_THM_CALL: the call to the local static helper() (same module).
 *   - .init_array   : mod_ctor (stamps .bss, proves ctor + placement).
 *
 * The test app loads relprov first, then relmod with a resolver that sees both
 * main's exports and relprov's — so prov_add binds to relprov's runtime address.
 */
#include <stdint.h>
#include "fm-iface.h"

extern uint32_t prov_add(uint32_t a, uint32_t b);   /* UND -> relprov */

uint32_t       rel_data  = 0x1100;          /**< .data, exported. */
const uint32_t rel_const = 0x00C0;          /**< .rodata, exported. */
static uint32_t rel_bss;                    /**< .bss (ctor stamps 0x00C1). */
const char*    rel_tag   = "reloc: hello";  /**< ABS32 -> .rodata string. */

/** @brief Constructor -> .init_array. */
__attribute__((constructor))
static void mod_ctor(void) {
    rel_bss = 0x00C1;
    trace_event("relmod", "ctor", rel_bss);
}

/** @brief Local static helper — kept out-of-line so the intra-module call is a
 *         real relocation (THM_CALL if in BL range, else an ABS32 long-call). */
__attribute__((noinline)) static uint32_t helper(uint32_t x) {
    return x * 3u + rel_const;
}

/** @brief Exported entry the test app calls. Deterministic for a given x:
 *         helper(x) + prov_add(x, rel_data) + fm_callback(rel_bss) + rel_bss. */
uint32_t rel_compute(uint32_t x) {
    trace_event("relmod", "rel_compute", x);      /* -> main   (ABS32 funcptr) */
    uint32_t h = helper(x);                        /* -> local  (THM_CALL)      */
    uint32_t p = prov_add(x, rel_data);            /* -> relprov(cross-module)  */
    uint32_t f = fm_callback(rel_bss);             /* -> main   (callback)      */
    trace_u32("  relmod helper  = ", h);
    trace_u32("  relmod prov    = ", p);
    trace_u32("  relmod fmcb    = ", f);
    return h + p + f + rel_bss;
}

/** @brief Exported data accessor (reads its own .data). */
uint32_t rel_read_data(void) { return rel_data; }

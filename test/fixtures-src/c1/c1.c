/**
 * @file c1.c
 * @brief c1 — a C loadable library with a static "class" (file-scope struct +
 *        constructor), a .data global, and a .bss global. See c1.h for BUILD.
 *
 * Relocation coverage: the FM imports (trace_event/trace_u32/fm_callback/fm_var/
 * fm_info) and the c2 imports (c2_add/c2_state) are left UND -> the loader resolves
 * them (functions: R_ARM_JUMP_SLOT; data: R_ARM_GLOB_DAT). Cross-lib JUMP_SLOTs to
 * c2 are re-routed through dl_cross_call so c2 runs with c2's own r10/got_base. The
 * "Hello" string and the constructor pointer take R_ARM_RELATIVE.
 */
#include "c1.h"
#include "fm-iface.h"   /* FM-exported trace + data + callback */

/* ---- Imports from c2 (defined in c2.so, UND here -> cross-lib JUMP_SLOT). ---- */
extern uint32_t c2_add(uint32_t a, uint32_t b);
extern uint32_t c2_state(void);

/* ---- "static class": a file-scope struct instance initialised by a constructor. */
struct C1Obj {
    uint32_t magic;   /**< constructor stamps 0x00C1. */
    uint32_t count;   /**< bumped on each cross call (proves .data/.bss reached). */
};
static struct C1Obj c1_obj;     /* .bss until the constructor runs */

/* ---- Plain data/bss globals (exported). ---- */
uint32_t c1_data = 0x1100;      /* .data  */
uint32_t c1_bss;                /* .bss   */
char*    c1_tag = "c1: hello";  /* string literal -> R_ARM_RELATIVE */

/* ---- Constructor -> .init_array, run by the loader at dlopen. ---- */
__attribute__((constructor)) static void c1_ctor(void) {
    c1_obj.magic = 0x00C1;
    c1_obj.count = 1;
    c1_bss = c1_data + 0x11;     /* 0x1111 */
    trace_event("c1", "c1_ctor(bss)", c1_bss);
}

/* Test 1: FM -> c1 -> c2. c1 touches its own state (needs c1's r10) then calls
 * c2_add cross-lib (needs c2's r10 -> dl_cross_call swaps it). */
uint32_t c1_call_c2(uint32_t x) {
    trace_event("c1", "c1_call_c2", x);
    c1_obj.count += 1;                          /* own .data/.bss access */
    uint32_t r = c2_add(x, c1_data);            /* cross-lib -> c2 */
    trace_u32("c1: c2_add -> ", r);
    return r + c1_obj.count + c1_bss;
}

/* Test 2: called BY c2 (cross-lib), then calls back into FM (main is r10-free). */
uint32_t c1_into_fm(uint32_t x) {
    trace_event("c1", "c1_into_fm", x);
    c1_obj.count += 1;
    uint32_t r = fm_callback(x + c1_data);      /* -> FM */
    trace_u32("c1: fm_callback -> ", r);
    return r + c1_obj.magic;
}

/* Test 5: c1 consumes FM-exported variable + struct ("class") + own ctor state. */
uint32_t c1_use_fm(void) {
    trace_event("c1", "c1_use_fm(fm_var)", fm_var);
    trace_u32("c1: fm_info.id = ", fm_info.id);
    return fm_var + fm_info.id + fm_info.flags + c1_obj.magic;
}

uint32_t c1_state(void) { return c1_obj.count; }

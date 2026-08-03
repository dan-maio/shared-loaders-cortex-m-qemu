/**
 * @file c2.c
 * @brief c2 — a C loadable library with a static "class" (file-scope struct +
 *        constructor), a .data global, a .rodata const, and a .bss global.
 *        See c2.h for BUILD.
 *
 * Relocation coverage: trace imports from FM (JUMP_SLOT), cross-lib imports
 * c1_into_fm (-> c1) and cpp3_step (-> cpp3) become cross-lib JUMP_SLOTs routed
 * through dl_cross_call. c2_data/c2_const are exported for FM to read in test 4.
 */
#include "c2.h"
#include "fm-iface.h"

/* ---- Cross-lib imports (UND here). ---- */
extern uint32_t c1_into_fm(uint32_t x);   /* defined in c1.so */
extern uint32_t cpp3_step(uint32_t x);    /* defined in cpp3.so */
extern uint32_t cpp3_recurse(uint32_t d); /* defined in cpp3.so (test 14) */

/* ---- "static class": file-scope struct instance initialised by a constructor. */
struct C2Obj {
    uint32_t tag;    /**< constructor stamps 0x00C2. */
    uint32_t hits;   /**< bumped on each entry (proves .bss reached via own r10). */
};
static struct C2Obj c2_obj;       /* .bss until constructed */

/* ---- Exported data (test 4). ---- */
uint32_t       c2_data  = 0x2200;     /* .data   */
const uint32_t c2_const = 0x02C0;     /* .rodata */
uint32_t       c2_bss;                /* .bss    */
char*          c2_tag = "c2: hello";  /* R_ARM_RELATIVE */

__attribute__((constructor)) static void c2_ctor(void) {
    c2_obj.tag  = 0x00C2;
    c2_obj.hits = 0;
    c2_bss = c2_data + 0x22;           /* 0x2222 */
    trace_event("c2", "c2_ctor(bss)", c2_bss);
}

/* Test 1: called cross-lib BY c1. Touches own .bss/.data (correct only if r10 was
 * swapped to c2's got_base by dl_cross_call). */
uint32_t c2_add(uint32_t a, uint32_t b) {
    c2_obj.hits += 1;
    trace_event("c2", "c2_add", a + b);
    return a + b + c2_obj.hits + c2_bss;     /* 0x2222 contribution proves .bss */
}

/* Test 2: FM -> c2 -> c1 -> FM. c2 calls c1_into_fm cross-lib. */
uint32_t c2_chain(uint32_t x) {
    trace_event("c2", "c2_chain", x);
    c2_obj.hits += 1;
    uint32_t r = c1_into_fm(x + c2_data);     /* cross-lib -> c1 */
    trace_u32("c2: c1_into_fm -> ", r);
    return r + c2_obj.tag;
}

/* Test 6 hop: cpp3 -> c2 -> cpp3. c2 calls cpp3_step cross-lib. */
uint32_t c2_relay(uint32_t x) {
    trace_event("c2", "c2_relay", x);
    c2_obj.hits += 1;
    uint32_t r = cpp3_step(x + c2_bss);       /* cross-lib -> cpp3 */
    trace_u32("c2: cpp3_step -> ", r);
    return r + c2_obj.hits;
}

/* Test 7: a parameter source FM reads to feed cpp3's method. */
uint32_t c2_param(void) {
    trace_event("c2", "c2_param", c2_data);
    return c2_data + c2_const;                /* 0x2200 + 0x02C0 = 0x24C0 */
}

/* Test 14: deep mutual cross-recursion c2 <-> cpp3. At each level the callee touches
 * its OWN .bss (hits/calls) via its OWN r10 -- a wrong r10 swap at any depth corrupts
 * or faults. Returns d + cpp3_recurse(d-1); the full chain sums to N(N+1)/2. */
uint32_t c2_step_down(uint32_t d) {
    c2_obj.hits += 1;                         /* own .bss (needs c2's r10) */
    trace_event("c2", "c2_step_down", d);
    if (d == 0) return 0;
    return d + cpp3_recurse(d - 1);           /* cross-lib -> cpp3 (r10 swaps back) */
}

/* Test 15: hard-float passed CROSS-LIB (cpp3 -> c2) through dl_cross_call. The float
 * arrives in s0; the trampoline must preserve d0-d7 across its r10 swap. Reading
 * c2_data (own .data, via r10) also proves the swap. Returns x + c2_data. */
float c2_scale(float x) {
    c2_obj.hits += 1;                         /* own .bss (needs c2's r10) */
    trace_u32("c2: c2_scale hits = ", c2_obj.hits);
    return x + (float)c2_data;                /* + 0x2200 (8704); proves s0 + r10 */
}

uint32_t c2_state(void) { return c2_obj.hits; }

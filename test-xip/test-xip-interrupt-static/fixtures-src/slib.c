/**
 * @file slib.c
 * @brief XIP fixture for the static test — full surface incl. imports from main.
 *
 * Built single-pic-base + split (xip-split.ld): the R-X segment executes from
 * flash, only the RW segment (GOT/.data/.bss) is copied to RAM (r10 = got_base).
 *
 * It exercises the whole XIP surface:
 *   - .data and .bss globals (RW segment, in RAM),
 *   - a constructor effect (pure/intra-lib only — no imported PLT call),
 *   - string literals reached through R_ARM_RELATIVE (flash .rodata),
 *   - a struct global with string members,
 *   - intra-lib calls (bound locally by -Wl,-Bsymbolic; direct BL),
 *   - a function-pointer path (a GOT relocation),
 *   - a DATA import from main (HOST_K)     -> GLOB_DAT, patched in the RAM GOT,
 *   - FUNCTION imports from main (host_f, printf) -> PLT/.got.plt in flash,
 *     patched to the cross-call trampoline at provision time.
 */
#include <stdint.h>

/* ---- imports from the application (resolved statically at load) ---------- */
extern unsigned int HOST_K;                 /* data import  (GLOB_DAT)        */
extern unsigned int host_f(unsigned int);   /* function import (PLT)          */
extern int          printf(const char*, ...);/* function import (PLT, newlib) */

/* ---- library-owned globals: .data + .bss land in the RW segment ---------- */
unsigned int s_a = 10;             /* .data  — mutated by the constructor      */
unsigned int s_b = 0;              /* .bss   — set by the constructor          */

const char* s_msg = "static-xip slib says hi";   /* pointer in RW -> flash    */

struct SInfo { const char* tag; unsigned int x; unsigned int y; };
struct SInfo s_info = { "slib-info", 7, 42 };

/* ---- constructor: pure/intra-lib work only ------------------------------- */
__attribute__((constructor)) static void s_ctor(void) {
    s_a += 5;            /* 10 -> 15 */
    s_b  = s_a * 2;      /* -> 30    */
}

/* ---- exported functions (execute from flash) ----------------------------- */
unsigned int s_add(unsigned int x, unsigned int y) { return x + y + s_a; }
unsigned int s_scale(unsigned int x)               { return x * s_info.y; }

static unsigned int s_helper(unsigned int x)       { return x + 100; }
unsigned int s_via_helper(unsigned int x) {          /* two intra-lib calls   */
    return s_helper(x) + s_add(x, 1);
}

const char*  s_text(void)  { return s_msg; }
const char*  s_tag(void)   { return s_info.tag; }
unsigned int s_get_a(void) { return s_a; }
unsigned int s_get_b(void) { return s_b; }

typedef unsigned int (*sfn)(unsigned int);
unsigned int s_apply(unsigned int x) {               /* fn pointer -> GOT      */
    sfn p = s_scale;
    return p(x) + 1;
}

/* ---- calls back into the application ------------------------------------- */
unsigned int s_use_host(unsigned int x) {            /* host_f (fn) + HOST_K (data) */
    return host_f(x) + HOST_K;
}
unsigned int s_mix(unsigned int x) {                 /* mutate .bss using an import   */
    s_b = s_add(x, 2) + host_f(x);
    return s_b;
}
void s_print(void) {                                 /* printf import from main       */
    printf("  [slib] from flash: s_a=%u s_b=%u HOST_K=%u\n", s_a, s_b, HOST_K);
}

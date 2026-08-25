/**
 * @file nslib.c
 * @brief Full-surface no-pic fixture: the same surface slib.c exercises for the
 *        r10 model, but compiled `-fno-pic -mlong-calls` at fixed addresses.
 *
 * Every reference below is absolute at link time — no GOT, no PLT, no r10:
 *   - .data / .bss globals         (RW segment, copied to the data window)
 *   - a constructor effect         (.init_array, called by the loader)
 *   - string literals              (in the R-X segment, executed from text window)
 *   - a struct global with strings
 *   - intra-lib calls              (plain BL, resolved by the linker)
 *   - an indirect call (fn pointer)
 *   - imports from main: HOST_K (data) and host_f / printf (functions) —
 *     emitted as R_ARM_ABS32 against undefined symbols, patched by the loader
 *     into the (RAM) text window at LOAD time.
 */
#include <stdint.h>

/* ---- imports from the application (resolved at load into the text window) */
extern unsigned int HOST_K;                  /* data import  (R_ARM_ABS32)   */
extern unsigned int host_f(unsigned int);    /* function import (R_ARM_ABS32) */
extern int          printf(const char*, ...);/* function import (newlib)      */

/* ---- library-owned globals: .data + .bss land in the RW (data) segment ---- */
unsigned int s_a = 10;             /* .data — mutated by the constructor      */
unsigned int s_b = 0;              /* .bss  — set by the constructor          */

const char* s_msg = "no-pic slib says hi";    /* pointer in RW -> text window  */

struct SInfo { const char* tag; unsigned int x; unsigned int y; };
struct SInfo s_info = { "nslib-info", 7, 42 };

/* ---- constructor: pure intra-lib work only ------------------------------- */
__attribute__((constructor)) static void s_ctor(void) {
    s_a += 5;            /* 10 -> 15 */
    s_b  = s_a * 2;      /* -> 30    */
}

/* ---- exported functions (execute from the text window) ------------------- */
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

typedef unsigned int (*sfn)(unsigned int, unsigned int);
unsigned int s_apply(unsigned int x) {               /* fn pointer -> indirect call */
    sfn p = s_add;
    return p(x, 1) + 1;
}

/* ---- calls back into the application ------------------------------------- */
unsigned int s_use_host(unsigned int x) {            /* host_f (fn) + HOST_K (data) */
    return host_f(x) + HOST_K;
}
unsigned int s_mix(unsigned int x) {                 /* mutate .bss using an import */
    s_b = s_add(x, 2) + host_f(x);
    return s_b;
}
void s_print(void) {                                 /* printf import from main    */
    printf("  [nslib] from text window: s_a=%u s_b=%u HOST_K=%u\n", s_a, s_b, HOST_K);
}

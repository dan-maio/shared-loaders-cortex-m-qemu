/**
 * @file c-test2.c
 * @brief C test library 2 — cross-call target, weak override, COPY test.
 *
 * Relocation types:
 *   R_ARM_GLOB_DAT  — extern globals from main (AA, BB, s1)
 *   R_ARM_JUMP_SLOT — calls to printf, c_test1_weak_func
 *   R_ARM_RELATIVE  — string pointers in .data
 *   R_ARM_ABS32     — constructor
 *   R_ARM_COPY      — extern char* s1 (copied from main's BSS)
 */
#include "c-test2.h"
#include <stdio.h>

/* ================================================================
 * Own globals — R_ARM_GLOB_DAT
 * ================================================================ */
int          ct2_val     = 999;
const char*  ct2_name    = "I am c_test2";

/* ================================================================
 * Static locals
 * ================================================================ */
static int   ct2_calls   = 0;

/* ================================================================
 * Externs from main — R_ARM_GLOB_DAT
 * ================================================================ */
extern int    AA;
extern int    BB;
extern char*  s1;

/* ================================================================
 * Constructor — R_ARM_ABS32
 * ================================================================ */
__attribute__((constructor))
void c_test2_init(void) {
    fprintf(stderr, "c_test2 constructor: ct2_val=%d\n", ct2_val);
}

/* ================================================================
 * Main function — cross-called from c_test1
 * ================================================================ */
int c_test2_func(int x) {
    ct2_calls++;
    /* Access extern AA from main (GLOB_DAT) and call printf (JUMP_SLOT) */
    printf("c_test2_func(%d): AA=%d ct2_val=%d\n", x, AA, ct2_val);
    return x * ct2_val + AA;
}

/* ================================================================
 * Own function
 * ================================================================ */
int c_test2_own_func(int x) {
    return x + BB;
}

/* ================================================================
 * Access s1 from main — tests GLOB_DAT for char* (or COPY)
 * ================================================================ */
void print_s1(void) {
    printf("c_test2: s1=\"%s\"\n", s1);
}

/* ================================================================
 * Override c_test1's weak function
 * ================================================================ */
int c_test1_weak_func(void) {
    return 200;  /* overrides c_test1's weak version */
}

/* ================================================================
 * Return self struct
 * ================================================================ */
int get_ct2_val(void) {
    return ct2_val;
}

int get_ct2_calls(void) {
    return ct2_calls;
}

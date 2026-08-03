/**
 * @file cov1.c
 * @brief Coverage lib 1 — RELATIVE, GLOB_DAT, ABS32, JUMP_SLOT cross-call
 *
 * Relocation coverage:
 *   R_ARM_RELATIVE  — string pointer (tag_str), struct CovResult defaults
 *   R_ARM_GLOB_DAT  — extern AA, BB, s1 from firmware
 *   R_ARM_ABS32     — constructor cov1_ctor in .init_array
 *   R_ARM_JUMP_SLOT — printf, cov2_func (cross-call to cov2)
 */
#include "cov1.h"
#include <stdio.h>

extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;

int   cov1_global  = 0;
float cov1_pi      = 0.0f;

static const char* tag_str = "COV1";

extern int cov2_func(int x);

/** @brief Return AA + 50 (GLOB_DAT test). */
int cov1_init_val(void) {
    return AA + 50;
}

/** @brief Double a value (no relocation). */
int cov1_double(int x) {
    return x * 2;
}

/** @brief Add AA and BB to x (GLOB_DAT). */
int cov1_add_globals(int x) {
    return x + AA + BB;
}

/** @brief Return struct with RELATIVE data pointer. */
struct CovResult cov1_get_result(int seed) {
    struct CovResult r;
    r.sum     = seed + AA + BB;
    r.value   = (float)(seed * 10);
    r.label   = tag_str;
    r.cross_val = 0;
    return r;
}

/** @brief Cross-call to cov2_func (JUMP_SLOT). */
int cov1_cross_call(int x) {
    int base = x + AA;
    int from_cov2 = cov2_func(x + 100);
    return base + from_cov2;
}

/** @brief Print via printf (JUMP_SLOT). */
void cov1_print(const char* msg) {
    printf("[COV1] %s | AA=%u BB=%u s1=\"%s\" cov1_global=%d\n",
           msg, AA, BB, s1, cov1_global);
}

/** @brief Constructor (ABS32 in .init_array). */
__attribute__((constructor)) void cov1_ctor(void) {
    cov1_global = AA * 2;
    cov1_pi = 3.14159f;
    printf("[COV1] constructor: AA=%u cov1_global=%d pi=%.5f\n",
           AA, cov1_global, (double)cov1_pi);
}

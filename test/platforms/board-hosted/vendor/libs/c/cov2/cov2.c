/**
 * @file cov2.c
 * @brief Coverage lib 2 — JUMP_SLOT (math/printf), RELATIVE, GLOB_DAT, cross-call to cov1
 *
 * Relocation coverage:
 *   R_ARM_JUMP_SLOT — printf, sinf, cosf, sqrtf via math library
 *   R_ARM_JUMP_SLOT — cov1_double (cross-call to cov1 via PLT)
 *   R_ARM_GLOB_DAT  — extern AA, BB, s1 from firmware
 *   R_ARM_RELATIVE  — string pointer (tag_str)
 *   R_ARM_ABS32     — constructor cov2_ctor
 */
#include "cov2.h"
#include <math.h>
#include <stdio.h>

extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;

int cov2_counter = 0;

static const char* tag_str = "COV2";

extern int cov1_double(int x);

/** @brief Called by cov1_cross_call (JUMP_SLOT). */
int cov2_func(int x) {
    cov2_counter++;
    return x + AA + BB;
}

/** @brief sinf+cosf+sqrtf chain (math JUMP_SLOT). */
float cov2_math_chain(float x) {
    float s = sinf(x);
    float c = cosf(x);
    float r = sqrtf(s * s + c * c);
    return r;
}

/** @brief Calls cov1_double (cross-lib JUMP_SLOT). */
int cov2_call_back(int x) {
    cov2_counter++;
    return cov1_double(x) + AA;
}

/** @brief Process array with sinf/cosf, struct return. */
struct Cov2Result cov2_process(int* data, int len) {
    struct Cov2Result r;
    r.math_val = 0.0f;
    r.count = 0;
    r.tag = tag_str;
    int i;
    for (i = 0; i < len; ++i) {
        r.math_val += sinf((float)data[i]) + cosf((float)data[i]);
        r.count += data[i];
    }
    return r;
}

/** @brief Print tag via printf (JUMP_SLOT). */
void cov2_print_tag(void) {
    printf("[COV2] tag=\"%s\" counter=%d AA=%u BB=%u s1=\"%s\"\n",
           tag_str, cov2_counter, AA, BB, s1);
}

/** @brief Constructor (ABS32 in .init_array). */
__attribute__((constructor)) void cov2_ctor(void) {
    cov2_counter = AA;
    printf("[COV2] constructor: AA=%u counter=%d  (ABS32)\n", AA, cov2_counter);
}

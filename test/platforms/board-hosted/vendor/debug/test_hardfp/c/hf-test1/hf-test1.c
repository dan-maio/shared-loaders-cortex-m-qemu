/**
 * @file hf-test1.c
 * @brief Hard-float test lib 1 — float ABI across library boundary.
 */
#include "hf-test1.h"
#include <math.h>
#include <stdio.h>

/* Extern globals from firmware */
extern int AA;
extern int BB;
extern float hf_pi;

float hf_add(float a, float b) {
    return a + b;                          /* hard-float: adds in FPU */
}

float hf_mul(float a, float b) {
    return a * b;                          /* hard-float: multiplies in FPU */
}

double hf_double_add(double a, double b) {
    return a + b;                          /* double in hard-float: uses FPU */
}

float hf_calc(float x, int n) {
    float r = x;
    for (int i = 0; i < n; i++)
        r = r * 0.5f + 1.0f;              /* iterative float ops */
    return r + AA;                         /* GLOB_DAT + float */
}

float hf_sincos(float x) {
    return sinf(x) + cosf(x);              /* math library (JUMP_SLOT) */
}

int hf_float_to_int(float f) {
    return (int)f;                         /* float→int conversion */
}

struct FloatResult hf_compute(float* arr, int n) {
    struct FloatResult r = {0, 1.0f, -1e10f, n};
    for (int i = 0; i < n; i++) {
        r.sum     += arr[i];
        r.product *= arr[i];
        if (arr[i] > r.max) r.max = arr[i];
    }
    return r;                              /* struct return with floats */
}

struct FloatResult hf_identity(struct FloatResult r) {
    return r;                              /* pass and return struct by value */
}

float hf_cross_call(float x) {
    return hf_test2_func(x) + 1.0f;        /* JUMP_SLOT: calls hf_test2 */
}

const char* hf_get_pi_str(void) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "hf_pi = %f", (double)hf_pi);
    return buf;                            /* RELATIVE: string in .data */
}

void hf_print_stats(void) {
    printf("hf_test1: AA=%d BB=%d hf_pi=%f\n", AA, BB, (double)hf_pi);
}

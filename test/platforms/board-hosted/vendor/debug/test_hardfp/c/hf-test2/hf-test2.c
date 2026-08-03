/**
 * @file hf-test2.c
 * @brief Hard-float test lib 2 — called by hf_test1.
 */
#include "hf-test2.h"
#include <stdio.h>

extern int AA;
extern float hf_pi;

float hf_test2_func(float x) {
    printf("hf_test2_func(%f): AA=%d\n", (double)x, AA);
    return x * 2.0f + AA;                 /* float op + GLOB_DAT */
}

float hf_test2_mul_add(float a, float b, float c) {
    return a * b + c;                     /* pure float ops */
}

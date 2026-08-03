/**
 * @file complex-test.c
 * @brief Complex test library with recursion, arrays, float math, structs.
 *
 * Exercises: R_ARM_JUMP_SLOT (sinf, cosf, sqrtf), R_ARM_RELATIVE (.data),
 *            R_ARM_GLOB_DAT (AA, s1), R_ARM_ABS32 (constructors),
 *            struct return, deep call chain, cross-function calls.
 */
#include "complex-test.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* Extern symbols from firmware (GLOB_DAT). */
extern unsigned int AA;
extern char*        s1;

/* Static data for R_ARM_RELATIVE */
static float scale = 2.0f;
static const char* tag = "[COMPLEX]";

/* ----------------------------------------------------------------- */
/* Constructor — ABS32 in .init_array */
/* ----------------------------------------------------------------- */
__attribute__((constructor)) void complex_init(void) {
    printf("%s test library initialized, AA=%u\n", tag, AA);
}

/* ----------------------------------------------------------------- */
/* Recursive Collatz-like computation (depth-controlled).            */
/* Recursion tree: at each level splits into 1-2 branches,          */
/* total calls ~ 2^depth (but capped by early termination).         */
/* ----------------------------------------------------------------- */
static int collatz_step(int n, int depth, int* calls) {
    (*calls)++;
    if (depth <= 0 || n <= 1) return n + AA;
    if (n % 3 == 0)
        return collatz_step(n / 3,     depth - 1, calls) + n;
    if (n % 3 == 1)
        return collatz_step(n * 2 + 1, depth - 1, calls) - n;
    /* n % 3 == 2: dual-branch recursion */
    int a = collatz_step(n - 1,        depth - 1, calls);
    int b = collatz_step(n * 2,        depth - 2, calls);
    return a + b + n;
}

int recursive_collatz(int n, int depth) {
    int calls = 0;
    int result = collatz_step(n, depth, &calls);
    printf("%s recursive_collatz(%d,%d)=%d (calls=%d)\n", tag, n, depth, result, calls);
    return result;
}

/* ----------------------------------------------------------------- */
/* Array processing: nested loops with float math.                   */
/* Multiplies each element, accumulates sum/product, applies sinf.   */
/* ----------------------------------------------------------------- */
float process_array(float* data, int len, int multiplier) {
    if (!data || len <= 0) return 0.0f;
    float sum = 0.0f, prod = 1.0f;
    int i, j;
    /* Outer loop: iterate array. */
    for (i = 0; i < len; ++i) {
        float v = data[i] * (float)multiplier;
        /* Inner loop: apply sinf/cosf chain. */
        for (j = 0; j < 3; ++j) {
            v = sinf(v) + cosf(v) + 1.0f;
        }
        sum += v;
        prod *= (v + 0.5f);
        data[i] = v;
    }
    printf("%s process_array: sum=%.2f prod=%.2f (len=%d mul=%d)\n",
           tag, (double)sum, (double)prod, len, multiplier);
    return sum;
}

/* ----------------------------------------------------------------- */
/* sqrt-based normalization on a copy of data (returns struct).      */
/* ----------------------------------------------------------------- */
static struct ComplexResult compute_stats(float* data, int len) {
    struct ComplexResult r;
    r.rec_val = 0;
    r.sum = 0.0f; r.product = 1.0f;
    r.iterations = 0;
    r.math_val = 0.0f;
    int i;
    for (i = 0; i < len; ++i) {
        float v = data[i];
        if (v > 0.0f) {
            float root = sqrtf(v);
            r.sum += root;
            r.product *= root;
            r.iterations++;
            r.math_val += sinf(root) + cosf(root);
        }
    }
    return r;
}

/* ----------------------------------------------------------------- */
/* Entry point: ties recursion + array processing + struct return.    */
/* ----------------------------------------------------------------- */
struct ComplexResult run_complex_test(int start, float* input, int len, int mul) {
    printf("%s run_complex_test(start=%d, len=%d, mul=%d)\n", tag, start, len, mul);

    /* Step 1: recursion */
    int rec = recursive_collatz(start, 5);

    /* Step 2: array processing (modifies input in-place) */
    float arr_sum = process_array(input, len, mul);

    /* Step 3: stats on the processed array */
    struct ComplexResult stats = compute_stats(input, len);
    stats.rec_val = rec;
    stats.sum += arr_sum;

    /* Step 4: string + extern access */
    printf("%s s1=\"%s\" AA=%u\n", tag, s1, AA);

    return stats;
}

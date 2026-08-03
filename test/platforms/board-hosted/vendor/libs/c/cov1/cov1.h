/**
 * @file cov1.h
 * @brief Coverage library 1: RELATIVE, GLOB_DAT, ABS32, JUMP_SLOT (cross-call to cov2).
 *
 * Exercises 3 of the 5 ARM relocation types. Works in tandem with cov2.so
 * to provide bidirectional cross-library call coverage.
 */
#ifndef COV1_H
#define COV1_H

/**
 * @struct CovResult
 * @brief Result struct with pointer member — tests R_ARM_RELATIVE.
 */
struct CovResult {
    int         sum;       /**< @brief Computed sum. */
    float       value;     /**< @brief Float value. */
    const char* label;     /**< @brief Pointer to string (R_ARM_RELATIVE). */
    int         cross_val; /**< @brief Value from cross-library call. */
};

extern int   cov1_global; /**< @brief Global variable (R_ARM_GLOB_DAT for AA*2). */
extern float cov1_pi;     /**< @brief Float constant (R_ARM_RELATIVE). */

/**
 * @brief Return AA + 50 — tests R_ARM_GLOB_DAT (reads extern AA).
 * @return AA + 50.
 */
int   cov1_init_val(void);

/**
 * @brief Double the input — no external refs.
 * @param x Input value.
 * @return x * 2.
 */
int   cov1_double(int x);

/**
 * @brief Add AA + BB + x — tests multiple GLOB_DAT reads.
 * @param x Input value.
 * @return AA + BB + x.
 */
int   cov1_add_globals(int x);

/**
 * @brief Return a struct with pointer member — tests R_ARM_RELATIVE.
 * @param seed Seed value for computation.
 * @return CovResult with label pointing to .rodata.
 */
struct CovResult cov1_get_result(int seed);

/**
 * @brief Cross-call to cov2_func — tests R_ARM_JUMP_SLOT across libraries.
 * @param x Input value.
 * @return cov2_func(x) * 2.
 */
int   cov1_cross_call(int x);

/**
 * @brief Print a message via printf — tests JUMP_SLOT for printf.
 * @param msg Message to print.
 */
void  cov1_print(const char* msg);

/**
 * @brief Constructor — tests R_ARM_ABS32 in .init_array.
 */
__attribute__((constructor)) void cov1_ctor(void);

#endif

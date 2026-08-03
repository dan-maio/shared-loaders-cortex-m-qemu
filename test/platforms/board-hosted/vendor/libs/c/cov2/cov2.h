/**
 * @file cov2.h
 * @brief Coverage library 2: JUMP_SLOT (math/printf), RELATIVE, GLOB_DAT, cross-call to cov1.
 *
 * Exercises the other 3 ARM relocation types (complementary to cov1.so).
 * Provides bidirectional cross-library call coverage.
 */
#ifndef COV2_H
#define COV2_H

/**
 * @struct Cov2Result
 * @brief Result struct with pointer member — tests R_ARM_RELATIVE.
 */
struct Cov2Result {
    float       math_val; /**< @brief Computed float value. */
    int         count;    /**< @brief Element count. */
    const char* tag;      /**< @brief Pointer to string (R_ARM_RELATIVE). */
};

extern int cov2_counter; /**< @brief Global counter (R_ARM_GLOB_DAT). */

/**
 * @brief Function called by cov1's cross_call — tests cross-lib JUMP_SLOT.
 * @param x Input value.
 * @return x * 26.
 */
int   cov2_func(int x);

/**
 * @brief Chain sinf+cosf+sqrtf — tests math library JUMP_SLOT.
 * @param x Input value.
 * @return sinf(x) + cosf(x) + sqrtf(x).
 */
float cov2_math_chain(float x);

/**
 * @brief Call back into cov1_double — tests reverse cross-lib JUMP_SLOT.
 * @param x Input value.
 * @return cov1_double(x) + 10.
 */
int   cov2_call_back(int x);

/**
 * @brief Process an array with math — tests RELATIVE + JUMP_SLOT.
 * @param data Pointer to int array.
 * @param len Number of elements.
 * @return Cov2Result with computed values.
 */
struct Cov2Result cov2_process(int* data, int len);

/**
 * @brief Print tag via printf — tests JUMP_SLOT for printf.
 */
void  cov2_print_tag(void);

/**
 * @brief Constructor — tests R_ARM_ABS32 in .init_array.
 */
__attribute__((constructor)) void cov2_ctor(void);

#endif

/**
 * @file complex-test.h
 * @brief Complex test library: recursion, arrays, float math, structs.
 *
 * Tests advanced scenarios: recursive Collatz sequence, array processing
 * with sinf/cosf, struct return by value, math library linkage.
 */
#ifndef COMPLEX_TEST_H
#define COMPLEX_TEST_H

/**
 * @struct ComplexResult
 * @brief Result struct — tests R_ARM_RELATIVE for pointer members.
 */
struct ComplexResult {
    int   rec_val;     /**< @brief Recursive computation result. */
    float sum;         /**< @brief Sum of processed array. */
    float product;     /**< @brief Product of processed array. */
    int   iterations;  /**< @brief Number of iterations performed. */
    float math_val;    /**< @brief Math library computation result. */
};

/**
 * @brief Recursive Collatz sequence — tests recursion through PLT.
 * @param n Starting value.
 * @param depth Maximum recursion depth.
 * @return Collatz result.
 */
int   recursive_collatz(int n, int depth);

/**
 * @brief Process an array with sinf/cosf — tests math library JUMP_SLOT.
 * @param data Pointer to float array.
 * @param len Number of elements.
 * @param multiplier Multiplier for each element.
 * @return Sum of processed elements.
 */
float process_array(float* data, int len, int multiplier);

/**
 * @brief Run full complex test — recursion + arrays + math + struct return.
 * @param start Starting value for recursion.
 * @param input Pointer to float array.
 * @param len Number of elements in array.
 * @param mul Multiplier for array processing.
 * @return ComplexResult with all computed values.
 */
struct ComplexResult run_complex_test(int start, float* input, int len, int mul);

#endif

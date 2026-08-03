/**
 * @file hf-test1.h
 * @brief Hard-float test lib 1: float ABI, cross-call, math.
 *
 * Tests hard-float ABI (-mfloat-abi=hard -mfpu=fpv4-sp-d16):
 * - Float arguments/returns via FPU registers (s0-s15, d0-d7)
 * - Cross-library calls with float arguments
 * - Math library (sinf, cosf) through PLT
 * - Struct with float members passed/returned by value
 */
#ifndef HF_TEST1_H
#define HF_TEST1_H

/**
 * @brief Float add — tests FPU register passing.
 * @param a First operand.
 * @param b Second operand.
 * @return a + b.
 */
float   hf_add(float a, float b);

/**
 * @brief Float multiply — tests FPU register passing.
 * @param a First operand.
 * @param b Second operand.
 * @return a * b.
 */
float   hf_mul(float a, float b);

/**
 * @brief Double add — tests D-register passing.
 * @param a First operand.
 * @param b Second operand.
 * @return a + b.
 */
double  hf_double_add(double a, double b);

/**
 * @brief Mixed int/float — tests FPU + integer register passing.
 * @param x Float value.
 * @param n Integer multiplier.
 * @return x * n.
 */
float   hf_calc(float x, int n);

/**
 * @brief sinf+cosf chain — tests math library JUMP_SLOT.
 * @param x Input value.
 * @return sinf(x) + cosf(x).
 */
float   hf_sincos(float x);

/**
 * @brief Float to int conversion.
 * @param f Float value.
 * @return Truncated int.
 */
int     hf_float_to_int(float f);

/**
 * @struct FloatResult
 * @brief Struct with floats — tests struct passing with FPU ABI.
 */
struct FloatResult {
    float sum;     /**< @brief Sum of array elements. */
    float product; /**< @brief Product of array elements. */
    float max;     /**< @brief Maximum element. */
    int   count;   /**< @brief Number of elements. */
};

/**
 * @brief Process float array — tests struct return with floats.
 * @param arr Pointer to float array.
 * @param n Number of elements.
 * @return FloatResult with computed values.
 */
struct FloatResult hf_compute(float* arr, int n);

/**
 * @brief Pass-through struct by value — tests struct passing with FPU ABI.
 * @param r Input FloatResult.
 * @return Copy of r.
 */
struct FloatResult hf_identity(struct FloatResult r);

extern float hf_test2_func(float x); /**< @brief Extern from hf_test2 (R_ARM_JUMP_SLOT). */

/**
 * @brief Cross-call to hf_test2 — tests cross-lib call with float.
 * @param x Float value.
 * @return hf_test2_func(x) + 1.0.
 */
float hf_cross_call(float x);

extern float hf_pi; /**< @brief Extern float from firmware (R_ARM_GLOB_DAT). */

/**
 * @brief Return pi as string — tests float access + string return.
 * @return Pointer to static string representation of hf_pi.
 */
const char* hf_get_pi_str(void);

#endif

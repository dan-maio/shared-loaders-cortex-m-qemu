/**
 * @file hf-test2.h
 * @brief Hard-float test lib 2 — cross-call target with float.
 *
 * Provides cross-call target for hf_test1 with float arguments/returns.
 */
#ifndef HF_TEST2_H
#define HF_TEST2_H

/**
 * @brief Cross-call target — tests float ABI across library boundary.
 * @param x Float value.
 * @return x * 2.0 + 1.0.
 */
float hf_test2_func(float x);

/**
 * @brief Multiply-add with 3 floats — tests multiple FPU register args.
 * @param a First operand.
 * @param b Second operand.
 * @param c Third operand.
 * @return a * b + c.
 */
float hf_test2_mul_add(float a, float b, float c);

#endif

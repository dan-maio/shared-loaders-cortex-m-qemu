/**
 * @file c-test2.h
 * @brief Public API for c_test2 — cross-call target + weak override.
 *
 * Provides cross-call target for c_test1 and weak function override
 * (c_test1_weak_func overrides c_test1's weak version).
 */
#ifndef C_TEST2_H
#define C_TEST2_H

/**
 * @brief Main cross-call target — tests R_ARM_JUMP_SLOT from c_test1.
 * @param x Input value.
 * @return x * ct2_val + AA.
 */
int c_test2_func(int x);

/**
 * @brief Own function — tests R_ARM_GLOB_DAT for BB.
 * @param x Input value.
 * @return x + BB.
 */
int c_test2_own_func(int x);

#endif

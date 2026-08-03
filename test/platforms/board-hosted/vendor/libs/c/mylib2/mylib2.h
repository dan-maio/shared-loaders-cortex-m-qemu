/**
 * @file mylib2.h
 * @brief Public API for mylib2 — a second C11 test shared library.
 *
 * Provides distinct exports (lf, lf2, lf3) so the two-library load
 * path and inter-library/main weak resolution stay covered.
 */
#ifndef MYLIB2_H_
#define MYLIB2_H_

extern unsigned int AA; /**< @brief Extern global from firmware (R_ARM_GLOB_DAT). */
extern unsigned int BB; /**< @brief Extern global from firmware (R_ARM_GLOB_DAT). */
extern char*        s1; /**< @brief Extern string pointer from firmware (R_ARM_GLOB_DAT). */

/**
 * @brief Basic library function — exercises R_ARM_JUMP_SLOT.
 * @param x Input value.
 * @return x * 2 + AA.
 */
unsigned int lf(unsigned int x);

/**
 * @brief Second function — independent JUMP_SLOT entry.
 * @param x Input value.
 * @return x + BB.
 */
unsigned int lf2(unsigned int x);

/**
 * @brief Third function — may call back into mylib1 (cross-lib).
 * @param x Input value.
 * @return Computed value.
 */
unsigned int lf3(unsigned int x);

/**
 * @brief Weak override target — overrides firmware's weak mm().
 * @return AA + AA.
 */
unsigned int mm(void);

/**
 * @brief Simple function returning a constant.
 * @return 99.
 */
unsigned int xxx(void);

/**
 * @brief Print library info via printf (R_ARM_JUMP_SLOT).
 */
void         pl(void);

/**
 * @brief Print library info with custom string.
 * @param str2 String to print.
 */
void         pl2(char* str2);

#endif /* MYLIB2_H_ */

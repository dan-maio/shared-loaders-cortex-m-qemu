/**
 * @file mylib2.h
 * @brief Public C11 API of mylib2 — the second loadable test library.
 *
 * mylib2 covers the two-library load path and weak resolution across the
 * main/lib boundary (`mm`), with its own distinct exports.
 */
#ifndef MYLIB2_H_
#define MYLIB2_H_

#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Return x + 3 + the exported global `aaa`. */
unsigned int lf(unsigned int x);
/** @brief Return x*3 - ccc + ddd. */
unsigned int lf2(unsigned int x);
/** @brief Call ::lf and ::lf2 plus the main-resolved external `w`. */
unsigned int lf3(unsigned int x);
/** @brief Print and return BB + AA (both externals from main). */
unsigned int xxx(void);
/** @brief Print internal state. */
void pl(void);
/** @brief Print @p s and the main-resolved external string `s1`. */
void pl2(char *s);
/** @brief Weak across the main/lib boundary (main also defines `mm`). */
#pragma weak mm
unsigned int mm(void);

#if defined(__cplusplus)
}
#endif

#endif /* MYLIB2_H_ */

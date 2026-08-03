/**
 * @file mylib2.c
 * @brief C11 test shared library — second fixture for two-library loading.
 *
 * Exercises:
 *   R_ARM_RELATIVE  — string pointer in .data
 *   R_ARM_GLOB_DAT  — exported globals (aaa, bbb) + externs (AA, BB, s1)
 *   R_ARM_JUMP_SLOT — cross-GOT calls (lf3 → lf, lf2)
 *   R_ARM_ABS32     — constructor in .init_array
 *   Weak resolution — mm() defined here (overrides main's weak default)
 *   Externals       — w() resolved from main
 */

#include "mylib2.h"
#include <stdio.h>

/* --- Exported globals --- */
unsigned int aaa = 10;
unsigned int XX  = 25;
unsigned int bbb;
static unsigned int ccc = 5;
static unsigned int ddd = 7;

/* --- Externals resolved from main --- */
extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;
extern unsigned int w(unsigned int);

/* Cross-library call: lf_cross calls g() from mylib1. */
extern unsigned int g(unsigned int);
/** @brief Cross-library call: invokes g() from mylib1, returns result+200. */
unsigned int lf_cross(unsigned int x) { return g(x) + 200; }

/* --- String pointer — R_ARM_RELATIVE --- */
char* str2 = "Hello mylib!";

/* ================================================================
 * Constructor — R_ARM_ABS32
 * ================================================================ */

__attribute__((constructor)) void pr(void) {
    aaa += aaa;
    bbb = aaa + 20;
    printf("I am a Constructor in Lib2!! %d \n", bbb + 10);
}

/** @brief Return x + 3 + aaa (aaa via GOT). */
unsigned int lf(unsigned int x) { return x + 3 + aaa; }

/** @brief Return x*3 - ccc + ddd (local static vars). */
unsigned int lf2(unsigned int x) { return x * 3 - ccc + ddd; }

/** @brief Call lf() + lf2() + w(). Two intra-lib JUMP_SLOTs + one external. */
unsigned int lf3(unsigned int x) {
    unsigned int a = lf(x);
    unsigned int b = lf2(x);
    bbb = a + b + w(x);
    return bbb + bbb;
}

/** @brief Print BB then return BB + AA. Both externals from main. */
unsigned int xxx(void) { printf("...%d.\n", BB); return BB + AA; }

/** @brief Decrement bbb and print. Exercises .data write + printf. */
void pl(void) { bbb -= 10; printf("String In Lib  %d\n", bbb); }

/** @brief Print string args, access s1 from main. */
void pl2(char* str2) { printf("in lib ...%s\n", str2); printf("in lib2 ...%s...\n", s1); }

/** @brief Static helper: add 20 to BB, call w() with AA. */
static void ww2(unsigned int x) { BB += 20; w(x + 2 + BB + AA); }

/** @brief Weak — returns 0 (overrides main's weak mm). */
unsigned int mm(void) { return 0; }

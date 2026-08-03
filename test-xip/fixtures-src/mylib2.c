/**
 * @file mylib2.c
 * @brief XIP variant of mylib2 — identical to test/fixtures-src/mylib2/mylib2.c
 *        except the constructor's printf is removed: a PLT call in a constructor
 *        runs at load time before r10/GOT routing is fully in effect, so XIP
 *        libraries must keep their constructors PLT-free (see mylib1.c).
 */
#include "mylib2.h"

/* ---- Externals resolved from main at load time. ---- */
extern unsigned int AA;               /**< Global defined in main. */
extern unsigned int BB;               /**< Global defined in main. */
extern char        *s1;               /**< String pointer defined in main. */
extern unsigned int w(unsigned int);  /**< Function defined in main. */

/* ---- Exported globals. ---- */
unsigned int aaa = 10;  /**< Initialised exported global. */
unsigned int xx  = 25;  /**< Initialised exported global. */
unsigned int bbb;       /**< Zero-init exported global (.bss). */

/* ---- Internal state (file-local .data). ---- */
static unsigned int ccc = 5;   /**< Constant addend used by lf2(). */
static unsigned int ddd = 7;   /**< Constant addend used by lf2(). */

/** @brief String literal -> R_ARM_RELATIVE. */
char *str2 = "Hello from mylib2!";

/* ---- Constructor. ---- */
__attribute__((constructor)) static void ctor(void) {
    aaa += aaa;
    bbb  = aaa + 20;
}

unsigned int lf(unsigned int x)  { return x + 3 + aaa; }
unsigned int lf2(unsigned int x) { return x * 3 - ccc + ddd; }

unsigned int lf3(unsigned int x) {
    unsigned int a = lf(x);
    unsigned int b = lf2(x);
    bbb = a + b + w(x);   /* w resolved from main */
    return bbb + bbb;
}

unsigned int xxx(void) { printf("mylib2 ...%u.\n", BB); return BB + AA; }

void pl(void)        { bbb -= 10; printf("mylib2 %u\n", bbb); }
void pl2(char *s)    { printf("mylib2 ...%s...%s\n", s, s1); }

/* ---- Weak: main also defines mm; loader resolves across the boundary. ---- */
unsigned int mm(void) { return 0; }

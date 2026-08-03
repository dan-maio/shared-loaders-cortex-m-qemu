/**
 * @file mylib1.c
 * @brief Implementation of mylib1 (see mylib1.h for the relocation coverage map).
 */
#include "mylib1.h"

#include <math.h>

/* ---- Externals resolved from main at load time (left undefined here). ---- */
extern unsigned int AA;               /**< Global defined in main.            */
extern unsigned int BB;               /**< Global defined in main.            */
extern char        *s1;               /**< String pointer defined in main.    */
extern unsigned int f(unsigned int);  /**< Function defined in main.          */
extern unsigned int w(unsigned int);  /**< Function defined in main.          */

/* ---- Exported globals (R_ARM_GLOB_DAT). ---- */
unsigned int aa = 10;  /**< Initialised exported global. */
unsigned int bb;       /**< Zero-init exported global (.bss). */

/* ---- Internal state (file-local). ---- */
static unsigned int cc   = 5;   /**< .data constant addend used by library_function2. */
static unsigned int dd   = 7;   /**< .data constant addend used by library_function2. */
static unsigned int bss1;       /**< .bss scratch mutated by library_function3/print_lib. */

/* ---- String literals -> pointers needing R_ARM_RELATIVE. ---- */
char *str  = "Hello mylib1!";
char *str_ = "Hello from mylib1!";

/** @brief Library-owned aggregate (string members exercise R_ARM_RELATIVE). */
struct Str_Lib s_lib = { "15_", 125.236f, "1235", 120, 30 };

/* ---- Constructors -> .init_array (collected into .inits by the ldscript). ---- */
__attribute__((constructor)) static void ctor_main(void) {
    aa += aa;
    bb  = aa + 20;
    printf("mylib1 constructor: bb=%u\n", bb);
}
__attribute__((constructor)) static void ctor_banner(void) {
    printf("mylib1 second constructor\n");
}

unsigned int library_function(unsigned int x)  { return x + 3 + aa; }
unsigned int library_function2(unsigned int x) { return x * 3 - cc + dd; }

unsigned int library_function3(unsigned int x) {
    unsigned int a = library_function(x);
    unsigned int b = library_function2(x);
    bb   = a + b + w(x);   /* w resolved from main */
    bss1 = bb + a;
    return bb + bss1;
}

unsigned int callback_f(unsigned int x) { return AA + f(aa + bb + x); }
unsigned int g(unsigned int x)          { return aa + bb + x; }
unsigned int ptrfunc(func p, unsigned int x) { return p(x) + x; }
unsigned int gg(unsigned int x)         { return x + ptrfunc(g, x); }
unsigned int fff(unsigned int x)        { return AA + BB + x; }

char *retstr(void)  { return str; }
char *retstr_(void) { return str_; }

void print_lib(void)        { bss1 = 10; printf("mylib1 %u %u\n", bb, bss1); }
void print_lib2(char *s)    { printf("mylib1 ...%s...%s\n", s, s1); }

struct Str_Lib ret_struct(struct Str_Lib stm) {
    struct Str_Lib r;
    r.a  = s_lib.a;
    r.bb = s_lib.bb;
    strcat(r.a, stm.a);
    strcat(r.a, "_12345");
    strcat(r.bb, stm.bb);
    r.b = stm.b + 10;
    r.c = stm.c * 2;
    r.f = stm.f * 6.2f + (float) log((double) stm.f);  /* log resolved from main */
    return r;
}
struct Str_Lib ret_struct_lib(void) { return s_lib; }

/* ---- Weak symbols: main may override; otherwise these defaults apply. ---- */
unsigned int foo(void)  { return 20; }
unsigned int boo(void)  { return 30; }
unsigned int boo2(void) { return boo() + 30; }
unsigned int lkj(void)  { return BB + 100; }

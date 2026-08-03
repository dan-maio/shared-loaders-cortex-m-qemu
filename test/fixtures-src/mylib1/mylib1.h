/**
 * @file mylib1.h
 * @brief Public C11 API of mylib1 — a loadable test shared library.
 *
 * mylib1 exercises the loader's relocation paths: exported globals
 * (R_ARM_GLOB_DAT), exported functions (R_ARM_JUMP_SLOT), string literals
 * (R_ARM_RELATIVE), constructors (.init_array / R_ARM_ABS32), externals
 * resolved from main, and weak symbols.
 */
#ifndef MYLIB1_H_
#define MYLIB1_H_

#include <stdio.h>
#include <string.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Aggregate returned/taken by value (exercises struct-return codegen). */
struct Str_Lib {
    char         *a;   /**< First string member.  */
    float         f;   /**< Float member.          */
    char         *bb;  /**< Second string member.  */
    unsigned int  b;   /**< First integer member.  */
    unsigned int  c;   /**< Second integer member. */
};

/** @brief Callback signature used by ::ptrfunc. */
typedef unsigned int (*func)(unsigned int);

/** @brief Return x + 3 + the exported global `aa`. */
unsigned int library_function(unsigned int x);
/** @brief Return x*3 - cc + dd using internal statics. */
unsigned int library_function2(unsigned int x);
/** @brief Call ::library_function and ::library_function2 (intra-lib GOT calls)
 *         plus the main-resolved external `w`. */
unsigned int library_function3(unsigned int x);
/** @brief Call the main-resolved externals `AA` and `f`. */
unsigned int callback_f(unsigned int x);
/** @brief Return aa + bb + x. */
unsigned int g(unsigned int x);
/** @brief Return x + ptrfunc(g, x). */
unsigned int gg(unsigned int x);
/** @brief Return AA + BB + x (both externals from main). */
unsigned int fff(unsigned int x);
/** @brief Invoke callback @p p with @p x and add x. */
unsigned int ptrfunc(func p, unsigned int x);
/** @brief Return the exported string `str`. */
char *retstr(void);
/** @brief Return the exported string `str_`. */
char *retstr_(void);
/** @brief Print internal state. */
void print_lib(void);
/** @brief Print @p s and the main-resolved external string `s1`. */
void print_lib2(char *s);
/** @brief Build a Str_Lib from @p stm and the library's own `s_lib`. */
struct Str_Lib ret_struct(struct Str_Lib stm);
/** @brief Return the library's own `s_lib`. */
struct Str_Lib ret_struct_lib(void);

/** @brief Weak: overridden by main if it defines `foo`. */
#pragma weak foo
unsigned int foo(void);
/** @brief Weak: overridden by main if it defines `boo`. */
#pragma weak boo
unsigned int boo(void);
/** @brief Weak: overridden by main if it defines `boo2`. */
#pragma weak boo2
unsigned int boo2(void);
/** @brief Weak across the main/lib boundary; returns BB + 100. */
unsigned int lkj(void);

#if defined(__cplusplus)
}
#endif

#endif /* MYLIB1_H_ */

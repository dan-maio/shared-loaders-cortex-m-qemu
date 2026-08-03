/**
 * @file mylib1.h
 * @brief Public API for mylib1 — a C11 test shared library.
 *
 * Every exported symbol exercises a specific ARM ELF relocation scenario
 * that the loader must handle (R_ARM_RELATIVE, GLOB_DAT, JUMP_SLOT,
 * ABS32, COPY, weak resolution, struct return).
 */
#ifndef MYLIB1_H_
#define MYLIB1_H_

#include <stddef.h>

/**
 * @struct Str_Lib
 * @brief Packed struct used to test struct-by-value return (R_ARM_RELATIVE).
 *
 * 52 bytes total. Contains char arrays, float, and unsigned ints.
 * Used to verify the loader handles large struct returns correctly.
 */
struct __attribute__((packed, aligned(4))) Str_Lib {
    char         a[16]; /**< @brief 16-byte char array. */
    float        f;     /**< @brief Float field. */
    char         bb[16]; /**< @brief 16-byte char array. */
    unsigned int b;     /**< @brief Unsigned int field. */
    unsigned int c;     /**< @brief Unsigned int field. */
};

/** @brief Function pointer type: takes unsigned int, returns unsigned int. */
typedef unsigned int (*func_p)(unsigned int);

extern unsigned int AA; /**< @brief Extern global from firmware (R_ARM_GLOB_DAT). */
extern unsigned int BB; /**< @brief Extern global from firmware (R_ARM_GLOB_DAT). */
extern char*        s1; /**< @brief Extern string pointer from firmware (R_ARM_GLOB_DAT). */

/**
 * @brief Basic library function — exercises R_ARM_JUMP_SLOT.
 * @param x Input value.
 * @return 3 * x + AA.
 */
unsigned int  library_function(unsigned int x);

/**
 * @brief Second library function — independent JUMP_SLOT entry.
 * @param x Input value.
 * @return x + BB.
 */
unsigned int  library_function2(unsigned int x);

/**
 * @brief Third library function — calls mylib2's lf_cross (cross-lib).
 * @param x Input value.
 * @return Computed value involving cross-library call.
 */
unsigned int  library_function3(unsigned int x);

/**
 * @brief Callback wrapper — exercises function pointer call through GOT.
 * @param x Input value passed to callback.
 * @return Result of callback(x) + 1.
 */
unsigned int  callback_f(unsigned int x);

/**
 * @brief Simple arithmetic function.
 * @param x Input value.
 * @return 2 * x + 5.
 */
unsigned int  g(unsigned int x);

/**
 * @brief Simple arithmetic function.
 * @param x Input value.
 * @return x + 10.
 */
unsigned int  fff(unsigned int x);

/**
 * @brief Return a string literal (R_ARM_RELATIVE for pointer in .data).
 * @return Pointer to static string "Hello from mylib1".
 */
char*         retstr(void);

/**
 * @brief Return a second string literal.
 * @return Pointer to static string "retstr_ works".
 */
char*         retstr_(void);

/**
 * @brief Call a function pointer — tests indirect call through GOT.
 * @param p Function pointer to call.
 * @param x Argument to pass to p.
 * @return p(x) + 1.
 */
unsigned int  ptrfunc(func_p p, unsigned x);

/**
 * @brief Calls g() internally — tests intra-library PLT call.
 * @param x Input value.
 * @return g(x) + 1.
 */
unsigned int  gg(unsigned int x);

/**
 * @brief Print library info via printf (R_ARM_JUMP_SLOT for printf).
 */
void          print_lib(void);

/**
 * @brief Print library info with custom string.
 * @param str2 String to print.
 */
void          print_lib2(char* str2);

/**
 * @brief Pass-through struct by value — tests R_ARM_RELATIVE in struct.
 * @param stm Input struct.
 * @return Copy of stm with b += 5.
 */
struct Str_Lib ret_struct(struct Str_Lib stm);

/**
 * @brief Return a new struct by value — tests R_ARM_RELATIVE.
 * @return New Str_Lib with b = 42.
 */
struct Str_Lib ret_struct_lib(void);

/** @weak foo — weak function, may be overridden by firmware. */
#pragma weak foo
unsigned int foo(void);

/** @weak boo — weak function, may be overridden by firmware. */
#pragma weak boo
unsigned int boo(void);

/** @weak boo2 — weak function, may be overridden by firmware. */
#pragma weak boo2
unsigned int boo2(void);

/**
 * @brief Simple function returning a constant.
 * @return 56.
 */
unsigned int lkj(void);

#endif /* MYLIB1_H_ */

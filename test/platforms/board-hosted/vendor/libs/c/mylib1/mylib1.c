/**
 * @file mylib1.c
 * @brief C11 test shared library — exercises every relocation scenario.
 *
 * Coverage per scenario:
 *   R_ARM_RELATIVE  — struct/string pointers in .data (s_lib, str, str_)
 *   R_ARM_GLOB_DAT  — exported globals (aa, bb, g) + externs (AA, BB, s1)
 *   R_ARM_JUMP_SLOT — cross-GOT calls (library_function3 → library_function2)
 *   R_ARM_ABS32     — __attribute__((constructor)) in .init_array
 *   R_ARM_COPY      — extern vars from main (AA, BB)
 *   Weak symbols    — foo, boo, boo2 (resolved to main's def if present)
 *   Externals       — f, w, AA, BB, s1 (resolved from main's symbol table)
 *   Struct return   — ret_struct / ret_struct_lib
 *   Math library    — log()
 */

#include "mylib1.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* --- Exported globals (.data) — R_ARM_RELATIVE + R_ARM_GLOB_DAT --- */
unsigned int aa = 10;
unsigned int bb;
static unsigned int cc = 5;
static unsigned int dd = 7;
static unsigned int bss1;
static unsigned int bss2;

/* --- Externals resolved from main — R_ARM_GLOB_DAT (value 0 in .so) --- */
extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;
extern unsigned int f(unsigned int);
extern unsigned int w(unsigned int);

/* For cross-library call test: g_cross calls lf() from mylib2. */
extern unsigned int lf_cross(unsigned int);

/** @brief Cross-library call: invokes lf_cross() from mylib2, returns result+100. */
unsigned int g_cross(unsigned int x) { return lf_cross(x) + 100; }

/* --- Local struct with pointer members — R_ARM_RELATIVE --- */
struct Str_Lib s_lib = { "15_", 125.236f, "1235", 120, 30 };

/* --- String pointers in .data — R_ARM_RELATIVE --- */
char* str  = "Hello mylib!";
char* str_ = "Hello from mylib!";

/* ================================================================
 * Constructors — R_ARM_ABS32 entries in .init_array
 * ================================================================ */

__attribute__((constructor)) void print_constr(void) {
    aa += aa;
    bb = aa + 20;
    printf("I am a Constructor!! %d \n", bb);
}

__attribute__((constructor)) void print_constr1(void) {
    printf("I am a Constructor1!!\n");
}

__attribute__((constructor)) void print_constr2(void) {
    printf("I am a Constructor2!!\n");
    printf("in Constructor2  %s\n", retstr());
}

__attribute__((constructor)) void print_constr3(void) {
    printf("I am a Constructor3!!\n");
}

/* ================================================================
 * Struct-returning functions — exercises ARM struct-return ABI
 * ================================================================ */

struct Str_Lib ret_struct(struct Str_Lib stm) {
    struct Str_Lib ssl;
    strcpy(ssl.a, s_lib.a);
    strcpy(ssl.bb, s_lib.bb);
    strcat(ssl.a, stm.a);
    strcat(ssl.a, "_12345");
    strcat(ssl.bb, stm.bb);
    ssl.b = stm.b + 10;
    ssl.c = stm.c * 2;
    ssl.f = stm.f;
    ssl.f = stm.f * 6.2f + logf((float)ssl.f);
    return ssl;
}

struct Str_Lib ret_struct_lib(void) {
    return s_lib;
}

/** @brief Add 3 + aa to x. Exercises R_ARM_GLOB_DAT for aa. */
unsigned int library_function(unsigned int x) { return x + 3 + aa; }

/** @brief Multiply x by 3, subtract cc, add dd. Exercises local static vars. */
unsigned int library_function2(unsigned int x) { return x * 3 - cc + dd; }

/** @brief Call library_function + library_function2 + w(). Two JUMP_SLOTs + one external. */
unsigned int library_function3(unsigned int x) {
    unsigned int a = library_function(x);
    unsigned int b = library_function2(x);
    bb = a + b + w(x);
    bss1 = bb + a;
    return bb + bss1;
}

/* Forward declaration of static helper used by callback_f. */
static void ww(unsigned int x);

/** @brief Call ww() then AA + f(aa + bb + x). Externals f,w from main. */
unsigned int callback_f(unsigned int x) {
    ww(x);
    return AA + f(aa + bb + x);
}

/** @brief Weak — returns 20. */
unsigned int foo(void) { return 20; }
/** @brief Weak — returns 30. */
unsigned int boo(void) { return 30; }
/** @brief Calls boo() + 30 (weak chain). */
unsigned int boo2(void) { return boo() + 30; }
/** @brief Returns BB + 100 (BB from main). */
unsigned int lkj(void) { return BB + 100; }

/** @brief Print global values via printf (external symbol). */
void print_lib(void) { bss2 = 10; printf("String In Lib %d %d\n", bb, bss2); }
/** @brief Print string args via printf, exercises external s1. */
void print_lib2(char* str2) { printf("in lib ...%s\n", str2); printf("in lib2 ...%s...\n", s1); }

/** @brief Static helper: add 20 to BB, call w(). */
static void ww(unsigned int x) { BB += 20; w(x + 2 + BB); }

/** @brief Return pointer to "Hello mylib!" (R_ARM_RELATIVE in .data). */
char* retstr(void)  { return str; }
/** @brief Return pointer to "Hello from mylib!". */
char* retstr_(void) { return str_; }

/** @brief Return aa + bb + x. Both globals accessed via GOT. */
unsigned int g(unsigned int x) { return aa + bb + x; }

/** @brief Return AA + BB + x (AA, BB from main). */
unsigned int fff(unsigned int x) { return AA + BB + x; }

/** @brief Call p(x) through function pointer, add x. Tests R_ARM_JUMP_SLOT for p. */
unsigned int ptrfunc(func_p p, unsigned x) { return p(x) + x; }

/** @brief Call ptrfunc(g, x). Two-level indirect: gg → ptrfunc → g. */
unsigned int gg(unsigned int x) { return x + ptrfunc(g, x); }

/**
 * @file c-test1.c
 * @brief C test library 1 — exercises relocation types:
 *   R_ARM_GLOB_DAT  — exported globals
 *   R_ARM_RELATIVE  — string/data pointers in .data
 *   R_ARM_JUMP_SLOT — calls to extern printf, c_test2_func
 *   R_ARM_ABS32     — constructor in .init_array
 *   R_ARM_COPY      — extern variables from main fallback
 */
#include "c-test1.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * Exported globals — R_ARM_GLOB_DAT
 * ================================================================ */
int          g_int       = 42;
int          g_int_arr[4] = { 10, 20, 30, 40 };
char*        g_str       = "Hello from c_test1!";
const char*  g_const_str = "Constant string in c_test1";

/* Self-referencing pointer: points to g_int itself → R_ARM_RELATIVE */
int* g_ptr_to_self = &g_int;

/* ================================================================
 * Static (local) globals — no relocation needed
 * ================================================================ */
static int    s_counter = 0;
static float  s_pi      = 3.14159f;

/* ================================================================
 * Struct with pointer members — R_ARM_RELATIVE inside
 * ================================================================ */
struct TestStruct g_test_struct = {
    .id    = 100,
    .name  = (char*)"TestStruct name",
    .val   = 2.718f,
    .arr   = { 1, 2, 3 },
    .label = "Struct label"
};

/* ================================================================
 * Constructor — R_ARM_ABS32 in .init_array
 * ================================================================ */
__attribute__((constructor))
void c_test1_init(void) {
    fprintf(stderr, "c_test1 constructor: g_int=%d\n", g_int);
    g_int += 10;  /* constructor modifies global */
}

/* ================================================================
 * Extern symbols resolved from firmware or addr_of() fallback
 * These test R_ARM_GLOB_DAT (data) and R_ARM_JUMP_SLOT (functions)
 * ================================================================ */
extern int    AA;            /* from firmware → GLOB_DAT */
extern int    BB;            /* from firmware → GLOB_DAT */
extern char*  s1;            /* from firmware → GLOB_DAT */
extern unsigned int f(unsigned int);  /* from firmware → JUMP_SLOT */
extern unsigned int w(unsigned int);  /* from firmware → JUMP_SLOT */

/* ================================================================
 * Functions
 * ================================================================ */

int get_int(void) {
    return g_int;            /* GLOB_DAT: address of g_int via GOT */
}

int add_ints(int a, int b) {
    return a + b;            /* no external refs */
}

int* get_int_ptr(void) {
    return &g_int;           /* GLOB_DAT: address via GOT */
}

char* get_str(void) {
    return g_str;            /* RELATIVE: pointer in .data */
}

void set_int(int v) {
    g_int = v;               /* GLOB_DAT: address + write */
}

int double_int(int x) {
    return x * 2;            /* pure register — no relocation */
}

struct TestStruct get_struct_copy(void) {
    return g_test_struct;    /* RELATIVE: struct with pointer members */
}

int sum_array(int* arr, int n) {
    int s = 0;
    for (int i = 0; i < n; i++)
        s += arr[i];
    return s;                /* no external refs — arr passed in */
}

int call_callback(int (*cb)(int), int x) {
    return cb(x) + 1;        /* function pointer call (no PLT) */
}

int add_ten_and_call(int x, int (*cb)(int)) {
    int y = add_ints(x, 10); /* local call (no PLT) */
    return cb(y);            /* function pointer call */
}

/* ================================================================
 * Cross-library call — R_ARM_JUMP_SLOT to c_test2
 * ================================================================ */
extern int c_test2_func(int x);  /* defined in c_test2.so */

int cross_call(int x) {
    return c_test2_func(x) + 1;  /* JUMP_SLOT + local add */
}

/* ================================================================
 * Extern firmware calls — R_ARM_JUMP_SLOT
 * ================================================================ */

/* Calls f() from firmware, adds g_int */
int call_f_from_firmware(int x) {
    return f((unsigned int)(x + g_int));  /* JUMP_SLOT + GLOB_DAT */
}

/* Calls w() from firmware, adds BB from firmware */
int call_w_with_BB(int x) {
    return w((unsigned int)(x + BB));     /* JUMP_SLOT + GLOB_DAT */
}

/* Accesses s1 from firmware — GLOB_DAT for char* */
void print_firmware_str(void) {
    printf("firmware s1 = %s\n", s1);    /* GLOB_DAT + JUMP_SLOT (printf) */
}

/* ================================================================
 * Weak symbols
 * ================================================================ */
#pragma weak weak_from_main
int weak_from_main(void);        /* overridden by main if present */

int call_weak(void) {
    int v = 10;
    if (weak_from_main)
        v = weak_from_main();
    return v + g_int;            /* GLOB_DAT */
}

/* Own weak — can be overridden by c_test2 */
#pragma weak c_test1_weak_func
int c_test1_weak_func(void) {
    return 100;
}

/* ================================================================
 * Static counter with get/set
 * ================================================================ */
int inc_counter(void) {
    return ++s_counter;
}

int get_counter(void) {
    return s_counter;
}

/* ================================================================
 * Float operations — tests soft-float library linkage
 * ================================================================ */
float get_pi(void) {
    return s_pi;
}

float circle_area(float radius) {
    return s_pi * radius * radius;
}

/* ================================================================
 * Call printf through GOT — R_ARM_JUMP_SLOT
 * ================================================================ */
void print_stats(void) {
    printf("c_test1: g_int=%d AA=%d BB=%d s_counter=%d\n",
           g_int, AA, BB, s_counter);   /* GLOB_DAT + JUMP_SLOT */
}

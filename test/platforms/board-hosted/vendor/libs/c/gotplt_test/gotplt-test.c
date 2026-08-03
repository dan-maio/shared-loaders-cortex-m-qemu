/**
 * @file gotplt-test.c
 * @brief GOT/PLT demonstration — every relocation type with trace output.
 *
 * Relocation coverage:
 *   R_ARM_RELATIVE  — string/data pointers in .data (label_str, block)
 *   R_ARM_GLOB_DAT  — externs got_aa, got_bb, got_s1
 *   R_ARM_JUMP_SLOT — printf, sinf, cosf, sqrtf, got_float_mul
 *   R_ARM_ABS32     — constructor in .init_array
 *   Internal PLT    — got_compute → got_add + got_mul
 */
#include "gotplt-test.h"
#include <math.h>
#include <stdio.h>

/* ====================================================================
 * Extern symbols from firmware — resolved via GLOB_DAT
 * ==================================================================== */
extern unsigned int AA;
extern unsigned int BB;
extern char*        s1;

unsigned int got_aa = 0;
unsigned int got_bb = 0;
char*        got_s1 = (char*)"";

/* ====================================================================
 * Data in .data section — resolved via RELATIVE
 * ==================================================================== */
static const char* label_str = "GOTPLT_test_lib";
static struct DataBlock default_block = { 42, 3.14f, "default" };

/* ====================================================================
 * Cross-call target: got_float_mul calls sqrtf (JUMP_SLOT to math lib)
 * ==================================================================== */
/** @brief Multiply then sqrt (math JUMP_SLOT). */
float got_float_mul(float a, float b) {
    float product = a * b;
    float root = sqrtf(product > 0 ? product : -product);
    return root;
}

/* ====================================================================
 * got_float_chain: sinf + cosf + got_float_mul — 3 JUMP_SLOTs
 * ==================================================================== */
/** @brief sinf+cosf+sqrtf (3 JUMP_SLOTs). */
float got_float_chain(float x) {
    float s = sinf(x);
    float c = cosf(x);
    float m = got_float_mul(s, c);
    return m;
}

/* ====================================================================
 * got_add / got_mul — simple arithmetic, no relocations
 * ==================================================================== */
/** @brief Add with AA (GLOB_DAT). */
int got_add(int a, int b) {
    return a + b + AA;
}

/** @brief Multiply with BB (GLOB_DAT). */
int got_mul(int a, int b) {
    return a * b + BB;
}

/* ====================================================================
 * got_compute — calls got_add + got_mul internally (JUMP_SLOTs within lib)
 * ==================================================================== */
/** @brief Internal PLT chain (add+mul). */
int got_compute(int x) {
    int a = got_add(x, 10);
    int b = got_mul(x, 3);
    return a + b;
}

/* ====================================================================
 * got_print — printf via JUMP_SLOT, shows got_aa + got_s1 via GLOB_DAT
 * ==================================================================== */
/** @brief Print via printf (JUMP_SLOT). */
void got_print(const char* msg) {
    printf("[GOTPLT] %s | got_aa=%u got_bb=%u got_s1=\"%s\" AA=%u BB=%u\n",
           msg, got_aa, got_bb, got_s1, AA, BB);
}

/* ====================================================================
 * got_call_chain — demonstrates 3-level call chain: main → got_call_chain
 *   → got_compute → got_add + got_mul
 * ==================================================================== */
/** @brief 3-level call chain through PLT. */
int got_call_chain(int x) {
    got_print("entering call_chain");
    int r = got_compute(x);
    got_print("leaving call_chain");
    return r;
}

/* ====================================================================
 * got_get_block — returns struct with data pointers (RELATIVE in return)
 * ==================================================================== */
/** @brief Return struct with RELATIVE pointer. */
struct DataBlock got_get_block(int id) {
    struct DataBlock b = default_block;
    b.id = id;
    b.value = (float)(id * 10);
    b.label = label_str;
    return b;
}

/* ====================================================================
 * got_show_block — prints struct fields, accesses extern s1 via GLOB_DAT
 * ==================================================================== */
/** @brief Print block fields (GLOB_DAT). */
void got_show_block(const struct DataBlock* b) {
    printf("[GOTPLT] block id=%d val=%.2f label=\"%s\" s1=\"%s\"\n",
           b->id, (double)b->value, b->label, s1);
}

/* ====================================================================
 * Constructor — ABS32 relocation in .init_array
 * Copies AA/BB/s1 values from firmware into local got_aa/got_bb/got_s1
 * ==================================================================== */
/** @brief Constructor (ABS32). */
__attribute__((constructor)) void gotplt_init(void) {
    got_aa = AA + 100;
    got_bb = BB + 200;
    got_s1 = s1;
    printf("[GOTPLT] constructor: AA=%u→got_aa=%u BB=%u→got_bb=%u s1=\"%s\"\n",
           AA, got_aa, BB, got_bb, s1);
}

/**
 * @file gotplt-test.h
 * @brief GOT/PLT demonstration library — exercises every relocation type.
 *
 * This library is designed to demonstrate all 5 ARM ELF relocation types:
 * - R_ARM_GLOB_DAT: got_aa, got_bb, got_s1 (extern globals)
 * - R_ARM_JUMP_SLOT: got_add, got_mul, got_compute, got_print (function calls)
 * - R_ARM_RELATIVE: got_get_block (struct with pointer member)
 * - R_ARM_ABS32: constructor in .init_array
 * - R_ARM_COPY: (if firmware defines AA, BB, s1)
 */
#ifndef GOTPLT_TEST_H
#define GOTPLT_TEST_H

/**
 * @struct DataBlock
 * @brief Struct with pointer member — tests R_ARM_RELATIVE.
 */
struct DataBlock {
    int         id;    /**< @brief Integer identifier. */
    float       value; /**< @brief Float value. */
    const char* label; /**< @brief Pointer to string (R_ARM_RELATIVE). */
};

extern unsigned int got_aa; /**< @brief Copy of AA at load time (R_ARM_GLOB_DAT). */
extern unsigned int got_bb; /**< @brief Copy of BB at load time (R_ARM_GLOB_DAT). */
extern char* got_s1;        /**< @brief Copy of s1 at load time (R_ARM_GLOB_DAT). */

/**
 * @brief Add two ints plus AA — tests R_ARM_JUMP_SLOT.
 * @param a First operand.
 * @param b Second operand.
 * @return a + b + AA.
 */
int   got_add(int a, int b);

/**
 * @brief Multiply two ints — tests R_ARM_JUMP_SLOT.
 * @param a First operand.
 * @param b Second operand.
 * @return a * b.
 */
int   got_mul(int a, int b);

/**
 * @brief Chain of add+mul calls — tests intra-library PLT.
 * @param x Input value.
 * @return got_add(x, got_mul(x, 2)).
 */
int   got_compute(int x);

/**
 * @brief Float multiply — tests float PLT.
 * @param a First operand.
 * @param b Second operand.
 * @return a * b.
 */
float got_float_mul(float a, float b);

/**
 * @brief Chain sinf+cosf+sqrtf — tests math library PLT.
 * @param x Input value.
 * @return sinf(x) + cosf(x) + sqrtf(x).
 */
float got_float_chain(float x);

/**
 * @brief Print a message via printf — tests JUMP_SLOT for printf.
 * @param msg Message to print.
 */
void  got_print(const char* msg);

/**
 * @brief Chain of function calls — tests intra-library call chain.
 * @param x Input value.
 * @return got_add(x, got_mul(x, 3)).
 */
int   got_call_chain(int x);

/**
 * @brief Return a struct with pointer member — tests R_ARM_RELATIVE.
 * @param id Identifier for the block.
 * @return DataBlock with label pointing to .rodata.
 */
struct DataBlock got_get_block(int id);

/**
 * @brief Print a DataBlock — tests struct access across GOT.
 * @param b Pointer to DataBlock to print.
 */
void  got_show_block(const struct DataBlock* b);

#endif

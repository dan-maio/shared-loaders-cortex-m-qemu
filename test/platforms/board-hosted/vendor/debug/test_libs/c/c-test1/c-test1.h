/**
 * @file c-test1.h
 * @brief Public API for c_test1 — exercises every C relocation scenario.
 *
 * Relocation types demonstrated:
 * - R_ARM_GLOB_DAT: g_int, g_int_arr, g_str, g_test_struct
 * - R_ARM_RELATIVE: string pointers, struct pointer members
 * - R_ARM_JUMP_SLOT: printf, c_test2_func, f(), w()
 * - R_ARM_ABS32: constructor c_test1_init in .init_array
 * - R_ARM_COPY: (if firmware defines AA, BB, s1)
 */
#ifndef C_TEST1_H
#define C_TEST1_H

#include <stddef.h>

extern int          g_int;           /**< @brief Global int (R_ARM_GLOB_DAT). */
extern int          g_int_arr[4];    /**< @brief Global int array (R_ARM_GLOB_DAT). */
extern char*        g_str;           /**< @brief Global string pointer (R_ARM_RELATIVE). */
extern const char*  g_const_str;     /**< @brief Global const string (R_ARM_RELATIVE). */
extern int*         g_ptr_to_self;   /**< @brief Self-referencing pointer (R_ARM_RELATIVE). */

/**
 * @struct TestStruct
 * @brief Struct with pointer members — tests R_ARM_RELATIVE inside struct.
 */
struct TestStruct {
    int         id;    /**< @brief Integer identifier. */
    char*       name;  /**< @brief Pointer to string (R_ARM_RELATIVE). */
    float       val;   /**< @brief Float value. */
    int         arr[3]; /**< @brief Integer array. */
    const char* label; /**< @brief Pointer to string (R_ARM_RELATIVE). */
};

extern struct TestStruct g_test_struct; /**< @brief Global struct (R_ARM_RELATIVE). */

/**
 * @brief Return g_int — tests R_ARM_GLOB_DAT read.
 * @return Current value of g_int.
 */
int         get_int(void);

/**
 * @brief Simple arithmetic — no external refs.
 * @param a First operand.
 * @param b Second operand.
 * @return a + b.
 */
int         add_ints(int a, int b);

/**
 * @brief Return pointer to g_int — tests R_ARM_GLOB_DAT.
 * @return Pointer to g_int.
 */
int*        get_int_ptr(void);

/**
 * @brief Return g_str — tests R_ARM_RELATIVE.
 * @return Pointer to static string.
 */
char*       get_str(void);

/**
 * @brief Write g_int — tests R_ARM_GLOB_DAT write.
 * @param v New value for g_int.
 */
void        set_int(int v);

/**
 * @brief Pure arithmetic — no external refs.
 * @param x Input value.
 * @return x * 2.
 */
int         double_int(int x);

/**
 * @brief Return struct copy — tests R_ARM_RELATIVE in struct.
 * @return Copy of g_test_struct.
 */
struct TestStruct get_struct_copy(void);

/**
 * @brief Sum an array — no external refs.
 * @param arr Pointer to int array.
 * @param n Number of elements.
 * @return Sum of elements.
 */
int         sum_array(int* arr, int n);

/**
 * @brief Call a function pointer — tests indirect call.
 * @param cb Function pointer to call.
 * @param x Argument to pass to cb.
 * @return cb(x) + 1.
 */
int         call_callback(int (*cb)(int), int x);

/**
 * @brief Local add + callback — tests intra-library + indirect call.
 * @param x Input value.
 * @param cb Function pointer to call.
 * @return cb(x + 10).
 */
int         add_ten_and_call(int x, int (*cb)(int));

extern int c_test2_func(int x); /**< @brief Extern from c_test2 (R_ARM_JUMP_SLOT). */

/**
 * @brief Cross-call to c_test2 — tests R_ARM_JUMP_SLOT across libraries.
 * @param x Input value.
 * @return c_test2_func(x) + 1.
 */
int         cross_call(int x);

/**
 * @brief Constructor — tests R_ARM_ABS32 in .init_array.
 */
void __attribute__((constructor)) c_test1_init(void);

#endif

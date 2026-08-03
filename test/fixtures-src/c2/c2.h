/**
 * @file c2.h
 * @brief c2 (C loadable library) public surface — Plan 6 cross-module battery.
 *
 * BUILD (see Makefile `LIB_CFLAGS`/`LIB_LDFLAGS`):
 *   compile: arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=hard
 *            -mfpu=fpv4-sp-d16 -fpic -msingle-pic-base -mno-pic-data-is-text-relative
 *            -mpic-register=r10 -std=c11 -Os -ffunction-sections
 *   link:    -shared -nostartfiles -Wl,-nmagic -Wl,-E
 *            -Xlinker --unresolved-symbols=ignore-all -T libs/ldscripts/gcc.ld
 *   start:   no entry point; .init_array (c2_ctor) runs at dlopen.
 */
#ifndef C2_H_
#define C2_H_
#include <stdint.h>

/** @brief Test 1: called cross-lib by c1; sums + own ctor state. */
uint32_t c2_add(uint32_t a, uint32_t b);
/** @brief Test 2 entry: FM -> c2 -> c1 -> FM. Calls c1_into_fm (cross-lib). */
uint32_t c2_chain(uint32_t x);
/** @brief Test 6 hop: cpp3 -> c2 -> cpp3. Calls cpp3_step (cross-lib). */
uint32_t c2_relay(uint32_t x);
/** @brief Test 7: a parameter source FM reads to feed cpp3. */
uint32_t c2_param(void);
/** @brief Observable internal-state probe (cross-call hit count). */
uint32_t c2_state(void);
/** @brief Test 14: deep mutual cross-recursion partner (c2 -> cpp3 -> c2 ...). */
uint32_t c2_step_down(uint32_t d);
/** @brief Test 15: hard-float scaled cross-lib (cpp3 -> c2); returns x + c2_data. */
float c2_scale(float x);

/* ---- Exported data FM consumes directly in test 4. ---- */
extern uint32_t       c2_data;    /**< .data global (FM reads by address). */
extern const uint32_t c2_const;   /**< .rodata const (FM reads by address). */

#endif /* C2_H_ */

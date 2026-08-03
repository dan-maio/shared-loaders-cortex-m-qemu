/**
 * @file c1.h
 * @brief c1 (C loadable library) public surface — Plan 6 cross-module battery.
 *
 * BUILD (see Makefile `LIB_CFLAGS`/`LIB_LDFLAGS`):
 *   compile: arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=hard
 *            -mfpu=fpv4-sp-d16 -fpic -msingle-pic-base -mno-pic-data-is-text-relative
 *            -mpic-register=r10 -std=c11 -Os -ffunction-sections
 *   link:    -shared -nostartfiles -Wl,-nmagic -Wl,-E
 *            -Xlinker --unresolved-symbols=ignore-all -T libs/ldscripts/gcc.ld
 *   start:   no entry point (e_entry=0); init runs via .init_array (c1_ctor),
 *            executed by the loader at dlopen.
 */
#ifndef C1_H_
#define C1_H_
#include <stdint.h>

/** @brief Test 1 entry: FM -> c1 -> c2. Calls c2_add (cross-lib), touches own state. */
uint32_t c1_call_c2(uint32_t x);
/** @brief Test 2 hop: called BY c2 (cross-lib), then calls back into FM. */
uint32_t c1_into_fm(uint32_t x);
/** @brief Test 5: c1 reads FM-exported var + struct, plus its own constructor state. */
uint32_t c1_use_fm(void);
/** @brief Observable constructor/state probe. */
uint32_t c1_state(void);

#endif /* C1_H_ */

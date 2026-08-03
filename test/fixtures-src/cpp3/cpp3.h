/**
 * @file cpp3.h
 * @brief cpp3 (C++ loadable library) C-ABI surface — Plan 6 cross-module battery.
 *
 * BUILD (see Makefile `CPP3_FLAGS`, the libcppsh recipe):
 *   compile+link: arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -mfloat-abi=hard
 *                 -mfpu=fpv4-sp-d16 -std=c++23 -fPIC -shared -fno-exceptions -fno-rtti
 *                 -fno-threadsafe-statics -fno-use-cxa-atexit -nostdlib -Os
 *                 -ffunction-sections
 *   start: no entry point; the C++ global object's constructor runs via .init_array
 *          at dlopen. The hard-float math (sinf/cosf/logf/sqrtf, single precision on
 *          fpv4-sp-d16) and operator new are left UND and resolved from main.
 */
#ifndef CPP3_H_
#define CPP3_H_
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Tests 3 & 7: FM ->(CM) cpp3. Constructs Mathy(seed) and returns its
 *         method result (hard-float sin/cos/log/sqrt, scaled to an integer). */
uint32_t cpp3_method(uint32_t seed, uint32_t x);
/** @brief Test 6 entry: FM -> cpp3 -> c2 -> cpp3 -> FM. Calls c2_relay (cross-lib). */
uint32_t cpp3_orchestrate(uint32_t x);
/** @brief Test 6 hop: called cross-lib BY c2, then calls back into FM. */
uint32_t cpp3_step(uint32_t x);
/** @brief Instances constructed (proves the ctor + .bss counter). */
uint32_t cpp3_calls(void);
/** @brief Banner-constructor stamp (proves .init_array ran). */
uint32_t cpp3_made(void);
/** @brief Test 14: deep mutual cross-recursion entry (cpp3 -> c2 -> cpp3 ...). */
uint32_t cpp3_recurse(uint32_t d);
/** @brief Test 15: passes a float cross-lib to c2_scale; returns (uint32_t)(x+c2_data). */
uint32_t cpp3_float_cross(uint32_t xi);
/** @brief Test 13: cross-module heap. Allocates with malloc/free + new/delete (all
 *         resolved to MAIN's heap) and returns ((sum of n words) ^ 0x55); (8)->533. */
uint32_t cpp3_heap(uint32_t n);

/* ---- Exported data FM consumes in test 6. ---- */
extern const uint32_t cpp3_magic;   /**< .rodata const. */
extern uint32_t       cpp3_var;     /**< .data global. */

#ifdef __cplusplus
}
#endif
#endif /* CPP3_H_ */

/**
 * @file pic-trampoline.h
 * @brief Enter a loaded library with r10 set to its GOT base.
 *
 * Single-pic-base libraries (mylib1/2) require r10 = their GOT base. main is
 * plain -fpic and does not use r10, so these trampolines may freely set it
 * (harmless for -fPIC libraries such as libcppsh). r10 is saved/restored.
 */
#ifndef FW_PIC_TRAMPOLINE_H_
#define FW_PIC_TRAMPOLINE_H_
#include <stdint.h>
#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Call void fn(void) with r10 = got. */
void     pic_call_void(void* fn, void* got);
/** @brief Call uint32_t fn(uint32_t a0) with r10 = got; returns its result. */
uint32_t pic_call_u32(void* fn, void* got, uint32_t a0);
/** @brief Call uint32_t fn(uint32_t a0, uint32_t a1) with r10 = got; returns result. */
uint32_t pic_call_u32_2(void* fn, void* got, uint32_t a0, uint32_t a1);

#if defined(__cplusplus)
}
#endif
#endif /* FW_PIC_TRAMPOLINE_H_ */

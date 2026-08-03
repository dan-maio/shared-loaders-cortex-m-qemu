/**
 * @file unwind-exidx.h
 * @brief Registry of loaded-library .ARM.exidx ranges for the C++ unwinder (Plan 5d).
 *
 * The ARM EHABI unwinder (linked into main from libgcc) calls
 * __gnu_Unwind_Find_exidx(pc, &count) to locate the unwind index table covering a
 * faulting PC. main provides a strong override (unwind-find-exidx.c) that consults
 * this registry: dlopen records each loaded image's *relocated* exidx range here so
 * a throw inside a loaded library can be unwound; dlclose removes it. PCs outside
 * any registered library fall back to main's own __exidx_start/__exidx_end.
 */
#ifndef UNWIND_EXIDX_H_
#define UNWIND_EXIDX_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register a loaded image's relocated .ARM.exidx range (no-op if full). */
void dl_exidx_register(uint32_t img_base, uint32_t img_size,
                       uint32_t exidx_base, int exidx_count);

/** @brief Remove the registration whose image base == @p img_base. */
void dl_exidx_unregister(uint32_t img_base);

#ifdef __cplusplus
}
#endif

#endif /* UNWIND_EXIDX_H_ */

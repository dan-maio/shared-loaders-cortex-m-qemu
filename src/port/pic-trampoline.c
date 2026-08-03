/** @file pic-trampoline.c @brief See pic-trampoline.h. */
#include "scl/pic-trampoline.h"

/* Arg regs on entry: r0=fn, r1=got, r2=a0, r3=a1. r12(ip) is a scratch register.
 * Params are consumed via the ABI registers inside naked asm, never by name.
 *
 * EHABI unwind directives (Plan 5d): each trampoline pushes {r10, lr}, so when a
 * loaded library throws and main catches, the C++ unwinder must step THROUGH this
 * frame (lib -> trampoline -> main). The .fnstart/.save {r10,lr}/.fnend directives
 * emit the correct .ARM.exidx descriptor ("pop {r10, r14}") so the unwinder restores
 * r10 and the caller return address; without them the naked-asm push is invisible to
 * the compiler and the unwinder would terminate (verified in docs/notes/plan5d-eh-spike.md).
 * main's own exidx (found via __gnu_Unwind_Find_exidx's fallback) covers these PCs. */
#define UNUSED __attribute__((unused))

__attribute__((naked)) void pic_call_void(void* fn UNUSED, void* got UNUSED) {
    __asm volatile(
        "  .fnstart           \n"
        "  push {r10, lr}      \n"
        "  .save {r10, lr}     \n"
        "  mov  r10, r1        \n"   /* r10 = GOT base */
        "  blx  r0             \n"   /* call fn() */
        "  pop  {r10, pc}      \n"
        "  .fnend              \n");
}

__attribute__((naked)) uint32_t pic_call_u32(void* fn UNUSED, void* got UNUSED, uint32_t a0 UNUSED) {
    __asm volatile(
        "  .fnstart           \n"
        "  push {r10, lr}      \n"
        "  .save {r10, lr}     \n"
        "  mov  r10, r1        \n"   /* r10 = GOT base */
        "  mov  r12, r0        \n"   /* ip = fn */
        "  mov  r0, r2         \n"   /* r0 = a0 */
        "  blx  r12            \n"   /* call fn(a0) */
        "  pop  {r10, pc}      \n"
        "  .fnend              \n");
}

__attribute__((naked)) uint32_t pic_call_u32_2(void* fn UNUSED, void* got UNUSED, uint32_t a0 UNUSED, uint32_t a1 UNUSED) {
    __asm volatile(
        "  .fnstart           \n"
        "  push {r10, lr}      \n"
        "  .save {r10, lr}     \n"
        "  mov  r10, r1        \n"   /* r10 = GOT base */
        "  mov  r12, r0        \n"   /* ip = fn */
        "  mov  r0, r2         \n"   /* r0 = a0 */
        "  mov  r1, r3         \n"   /* r1 = a1 */
        "  blx  r12            \n"   /* call fn(a0, a1) */
        "  pop  {r10, pc}      \n"
        "  .fnend              \n");
}

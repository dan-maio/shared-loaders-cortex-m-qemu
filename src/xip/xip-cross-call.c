/**
 * @file xip-cross-call.c
 * @brief Cross-library call trampoline: swaps the PIC base register (r10)
 *        to the CALLEE library's got_base for the duration of a lib->lib call.
 *
 * Why this exists: the libraries are built -msingle-pic-base -mpic-register=r10, so a
 * function reaches its own globals through r10 = *its* got_base. FM->lib entry sets r10
 * via the pic_call_* trampolines, but a lib->lib call (e.g. c1 -> c2) would otherwise run
 * the callee with the *caller's* r10 -> the callee reads the wrong .data/.bss.
 *
 * Mechanism: at dlopen, every EXTERNAL R_ARM_JUMP_SLOT (st_value==0) of a library whose
 * cross-call mode is enabled has its GOT slot pointed at this trampoline. The linker's
 * PLTn loads ip=&GOT[n] then `ldr pc,[GOT[n]]`, so we are entered with ip = &GOT[n]
 * (the slot identity), the target's args live in r0-r3 / d0-d7, and lr = caller return.
 * We call XIP_CROSS_LOOKUP(&GOT[n]) -> { target, callee_got } (packed in a u64), swap
 * r10 := callee_got around a `blx target`, then restore the caller's r10 and return.
 *
 * Compiled as C (not C++) so .fnstart/.fnend directives work without compiler conflicts.
 *
 * Parameters (preprocessor defines):
 *   XIP_CROSS_LOOKUP  — lookup function name (default: xip_cross_lookup).
 *                        For the nolib dynamic loader: dl_cross_lookup.
 *   XIP_CROSS_FN      — function name (default: xip_cross_call).
 *                        For the nolib dynamic loader: dl_cross_call.
 */
#include <stdint.h>

#ifndef XIP_CROSS_LOOKUP
#define XIP_CROSS_LOOKUP xip_cross_lookup
#endif
#ifndef XIP_CROSS_FN
#define XIP_CROSS_FN xip_cross_call
#endif

#define STRINGIFY2(x) #x
#define STRINGIFY(x)  STRINGIFY2(x)
#define CONCAT2(a, b) a ## _ ## b
#define CONCAT(a, b)  CONCAT2(a, b)

extern uint64_t XIP_CROSS_LOOKUP(uint32_t got_slot_addr);

/* d0-d7 carry floating-point arguments under the hard-float ABI and must survive
 * the lookup call. Under soft-float there are no FP argument registers — and no
 * FPU for the assembler to accept vpush/vpop against — so the pair is omitted.
 * The stack is balanced either way (vpop restores sp before the ldr offsets
 * below are used), so nothing else changes. __ARM_FP is defined by the compiler
 * exactly when hardware floating point is enabled. */
#if defined(__ARM_FP)
#  define XIP_VPUSH_D0_D7 "  vpush {d0-d7}                     \n"
#  define XIP_VPOP_D0_D7  "  vpop  {d0-d7}                     \n"
#else
#  define XIP_VPUSH_D0_D7 ""
#  define XIP_VPOP_D0_D7  ""
#endif

__attribute__((naked)) void XIP_CROSS_FN(void) {
    __asm volatile(
        "  .fnstart                          \n"
        "  push  {r4, r5, r6, r7, r10, lr}   \n"
        "  .save {r4, r5, r6, r7, r10, lr}   \n"
        "  push  {r0, r1, r2, r3}            \n"
        "  .pad  #16                         \n"
        XIP_VPUSH_D0_D7
        "  mov   r0, r12                      \n"
        "  bl    " STRINGIFY(XIP_CROSS_LOOKUP) " \n"
        "  mov   r4, r0                       \n"
        "  mov   r5, r1                       \n"
        XIP_VPOP_D0_D7
        "  ldr   r0, [sp, #0]                 \n"
        "  ldr   r1, [sp, #4]                 \n"
        "  ldr   r2, [sp, #8]                 \n"
        "  ldr   r3, [sp, #12]                \n"
        "  mov   r10, r5                      \n"
        "  blx   r4                           \n"
        "  add   sp, sp, #16                  \n"
        "  pop   {r4, r5, r6, r7, r10, pc}    \n"
        "  .fnend                             \n");
}

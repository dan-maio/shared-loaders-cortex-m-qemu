/**
 * @file lazy-resolve.c
 * @brief dl_runtime_resolve: the hand-written Thumb trampoline for 5c lazy binding.
 *
 * Entry contract (see docs/notes/plan5c-lazy-spike.md): an unbound JUMP_SLOT's GOT slot is
 * set by the loader to point straight at this trampoline (we deliberately bypass the linker's
 * PLT0, whose GOT-relative offset arithmetic is ambiguous on Cortex-M). The linker's PLTn does
 * `movw/movt/add ip,pc; ldr pc,[ip]`, so on entry ip(r12) = &GOT[n] (its `add ip,pc` is at a
 * word-aligned PC, so this is exact) and lr = the caller's return address. The target's original
 * args are live in r0-r3 and d0-d7.
 *
 * We save the args + lr, call dl_fixup(&GOT[n]) (which resolves the symbol, patches *GOT[n], and
 * returns target|1), restore them, and tail-call the target. r10 is left untouched: the lazy path
 * then behaves exactly like the eager path (PLTn -> patched slot), where the callee runs with the
 * caller's r10. main is plain -fpic, so dl_fixup itself needs no r10.
 */

/* push {r0-r3, r4, lr}: saves the integer args, the caller return (lr), and r4. The PLTn path
 * does not push, so sp is 8-aligned on entry; pushing 6 words (24 bytes) keeps it 8-aligned for
 * the AAPCS call to dl_fixup, and vpush {d0-d7} (64 bytes) keeps it there. r4 is a pad. */
/**
 * @brief Lazy-binding PLT trampoline (entered via an unbound GOT slot; see the
 *        file header). Calls dl_fixup(&GOT[n]) to resolve+patch, then tail-calls
 *        the target with the original args.
 */
__attribute__((naked)) void dl_runtime_resolve(void) {
    __asm volatile(
        "  push  {r0-r3, r4, lr} \n"  /* save int args + caller return (+ r4 pad for alignment) */
        "  vpush {d0-d7}         \n"  /* save VFP args (AAPCS hard-float) */
        "  mov   r0, r12         \n"  /* arg0 = &GOT[n] (ip set by PLTn) */
        "  bl    dl_fixup        \n"  /* r0 = resolved target | 1 */
        "  mov   r12, r0         \n"  /* ip = target */
        "  vpop  {d0-d7}         \n"  /* restore VFP args */
        "  pop   {r0-r3, r4, lr} \n"  /* restore int args + caller return; sp balanced */
        "  bx    r12             \n");/* tail-call target with original args + caller lr */
}

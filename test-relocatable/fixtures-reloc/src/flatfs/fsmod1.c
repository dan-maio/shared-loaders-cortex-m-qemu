/**
 * @file fsmod1.c
 * @brief A tiny "plugin" module stored in flash (flatfs demo). Self-contained:
 *        it only imports the host trace, so it loads standalone from a flash blob.
 */
#include <stdint.h>
extern void trace_u32(const char* label, unsigned v);   /* host import */

static uint32_t fs1_state = 1000;                        /* .data, per-load */

__attribute__((constructor)) static void fs1_ctor(void) {
    trace_u32("  [fsmod1] ctor state=", fs1_state);
}

uint32_t fsmod1_run(uint32_t x) { fs1_state += x; return fs1_state; }  /* stateful */
uint32_t fsmod1_id(void)        { return 0xF1u; }

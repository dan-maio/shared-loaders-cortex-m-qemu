/**
 * @file fsmod2.c
 * @brief A second flatfs plugin. Also self-contained (host trace only).
 */
#include <stdint.h>
extern void trace_u32(const char* label, unsigned v);   /* host import */

__attribute__((constructor)) static void fs2_ctor(void) {
    trace_u32("  [fsmod2] ctor id=", 0xF2u);
}

uint32_t fsmod2_square(uint32_t x) { return x * x; }
uint32_t fsmod2_id(void)           { return 0xF2u; }

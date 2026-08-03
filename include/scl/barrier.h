#pragma once

#if defined(__arm__) || defined(__thumb__)
#define scl_memory_barrier()                     \
    do {                                         \
        __asm volatile("dsb 0xf" ::: "memory"); \
        __asm volatile("isb 0xf" ::: "memory"); \
    } while (0)
#else
#define scl_memory_barrier() ((void)0)
#endif

/**
 * @file nprov.c
 * @brief Cross-module no-pic fixture: the PROVIDER.
 *
 * ncons.so imports nprov's `p_fn` (a R_ARM_ABS32 against the undefined symbol
 * `p_fn`); the loader resolves it at load time against the already-loaded
 * nprov module — a plain indirect call, no trampoline, no r10 swap.
 */
#include <stdint.h>

unsigned int p_base = 7;                     /* .data in the provider's window */

unsigned int p_fn(unsigned int x)             { return x * 3u + p_base; }
unsigned int p_tag(unsigned int x)            { return x + 1000u; }

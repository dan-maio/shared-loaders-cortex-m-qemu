/**
 * @file ncons.c
 * @brief Cross-module no-pic fixture: the CONSUMER.
 *
 * Imports `p_fn` from nprov.so. Built with `-L<fixtures> -l:nprov.so`, which
 * makes `p_fn` a known (but undefined-in-this-object) dynamic symbol; the
 * reference stays an R_ARM_ABS32 slot the loader fills with nprov's fixed
 * address when ncons is loaded AFTER nprov.
 */
#include <stdint.h>

extern unsigned int p_fn(unsigned int);

unsigned int c_fn(unsigned int x)  { return p_fn(x) + 1u; }   /* -> p_fn then +1 */
unsigned int c_double(unsigned int x) { return p_fn(p_fn(x)); } /* 3 p_fn calls  */

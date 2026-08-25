/**
 * @file nmylib1.c
 * @brief Second no-pic fixture: constructor effects, imports, reload proof.
 *
 * Same `-fno-pic -mlong-calls` fixed-address recipe as nslib.c.
 */
#include <stdint.h>

extern unsigned int HOST_K;
extern unsigned int host_f(unsigned int);

unsigned int aa = 10;               /* .data — constructor doubles it to 20  */
unsigned int bb = 0;                /* .bss  — constructor sets bb = aa + 20 */

__attribute__((constructor)) static void ctor(void) {
    aa = aa * 2;                    /* 10 -> 20 */
    bb = aa + 20;                   /* -> 40    */
}

unsigned int library_function(unsigned int x)  { return x + aa; }
unsigned int library_function2(unsigned int x) { return x + bb; }
unsigned int library_function3(unsigned int x) { return host_f(x) + aa; }
unsigned int get_aa(void)                      { return aa; }
unsigned int get_bb(void)                      { return bb; }

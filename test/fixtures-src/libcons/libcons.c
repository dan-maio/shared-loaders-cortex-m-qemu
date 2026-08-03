/** @file libcons.c @brief See libcons.h. prov_value is left UND -> resolved from libprov. */
#include "libcons.h"
extern unsigned int prov_value(unsigned int x);   /* defined in libprov, not here */
unsigned int cons_call(unsigned int x) { return prov_value(x) + 1; }

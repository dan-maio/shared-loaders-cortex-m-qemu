/**
 * @file tls_lib.c
 * @brief Thread-local test fixture. On arm-none-eabi, `__thread` lowers to
 * emulated TLS: a `__emutls_v.<name>` control struct (GLOB_DAT) whose `templ`
 * pointer needs a RELATIVE relocation, plus a call to `__emutls_get_address`
 * (JUMP_SLOT, resolved from main's libgcc). None of this needs the native
 * PT_TLS path — it is ordinary relocation handling the loader already does.
 */
#include "tls_lib.h"

__thread int tls_counter = 100;   /* initialised thread-local (.tdata template) */
__thread int tls_uninit;          /* zero-init thread-local (.tbss)           */

int tls_get(void)  { return tls_counter; }
int tls_bump(void) { tls_counter += 7; tls_uninit += 1; return tls_counter; }
int tls_zero(void) { return tls_uninit; }

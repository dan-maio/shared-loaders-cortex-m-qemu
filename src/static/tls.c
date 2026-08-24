/**
 * @file tls.c
 * @brief Single-thread TLS thread pointer for the scl::static loader.
 *
 * The soft-TP ABI (`-mtp=soft`, the ARM default) makes every TLS access call
 * `__aeabi_read_tp()` to obtain the thread pointer. We keep one global block
 * pointer: when the loader allocates a module's TLS block it publishes the base
 * here, and `__aeabi_read_tp()` (weak, overridable) returns it.
 *
 * This is deliberately the *single-thread, single-active-TLS-module* model:
 * one thread pointer for the whole system. A real multi-thread RTOS application
 * overrides `__aeabi_read_tp()` (and `scl_tls_set_pointer`) to return the
 * per-thread pointer instead.
 */
#include "static/tls.h"

static void* g_tls_pointer = 0;

void scl_tls_set_pointer(void* p) { g_tls_pointer = p; }
void* scl_tls_get_pointer(void)  { return g_tls_pointer; }

__attribute__((weak)) void* __aeabi_read_tp(void) { return g_tls_pointer; }

/**
 * @file log.c
 * @brief Weak no-op defaults for the SCL logging hook (see scl/log.h).
 */
#include "scl/log.h"

__attribute__((weak)) void scl_log_puts(const char* s) { (void)s; }
__attribute__((weak)) void scl_log_putc(char c) { (void)c; }

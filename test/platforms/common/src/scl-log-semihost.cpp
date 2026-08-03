/**
 * @file scl-log-semihost.cpp
 * @brief Test-app implementation of the SCL log hook: route loader/overlay
 *        tracing to the semihosting console (overrides the weak no-ops).
 */
#include "scl/log.h"
#include "semihost.h"

extern "C" void scl_log_puts(const char* s) { trace_puts(s); }
extern "C" void scl_log_putc(char c) { trace_putc(c); }

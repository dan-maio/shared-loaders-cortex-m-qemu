/** @file scl-log-trace.cpp @brief SCL log hook -> diag/trace semihosting. */
#include "scl/log.h"
#include "diag/trace.h"

extern "C" void scl_log_puts(const char* s) { trace_puts(s); }
extern "C" void scl_log_putc(char c) { trace_putchar(c); }

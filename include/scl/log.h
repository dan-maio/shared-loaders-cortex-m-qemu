/**
 * @file log.h
 * @brief SCL logging hook.
 *
 * The SCL never talks to a UART/semihosting directly. All diagnostic output is
 * routed through these two functions. Weak no-op defaults are provided in
 * src/shared/log.c; the application overrides them (e.g. with trace_puts /
 * trace_putc over semihosting) to enable loader tracing.
 */
#ifndef SCL_LOG_H_
#define SCL_LOG_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Emit a NUL-terminated string. Weak default: no-op. */
void scl_log_puts(const char* s);
/** @brief Emit a single character. Weak default: no-op. */
void scl_log_putc(char c);
/** @brief Print "label0xXXXXXXXX" (8 hex digits), no libc. */
void scl_log_put_hex(const char* label, uint32_t v);

#if defined(__cplusplus)
}
#endif
#endif /* SCL_LOG_H_ */

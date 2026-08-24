/**
 * @file tls_lib.h
 * @brief Thread-local test fixture (emulated TLS on arm-none-eabi).
 *
 * `__thread` compiles to emulated TLS (`__emutls_*`) on this toolchain: the
 * loader exercises it through ordinary GLOB_DAT/RELATIVE/JUMP_SLOT relocations.
 * See tls_lib.c.
 */
#ifndef TLS_LIB_H_
#define TLS_LIB_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Return the current thread's tls_counter (starts at 100). */
int tls_get(void);
/** Add 7 to tls_counter (and 1 to tls_zero), return the new value. */
int tls_bump(void);
/** Return the current thread's tls_zero (starts at 0). */
int tls_zero(void);

#ifdef __cplusplus
}
#endif

#endif /* TLS_LIB_H_ */

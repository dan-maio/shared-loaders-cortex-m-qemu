/**
 * @file cppeh.h
 * @brief Mangling-free public API of libcppeh (Plan 5d: C++ exceptions + RTTI).
 *
 * Implemented in C++23 with exceptions/RTTI ENABLED (unlike libcppsh), exported
 * as plain C names so the loader resolves them via dlsym without demangling.
 */
#ifndef CPPEH_H_
#define CPPEH_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/// @brief Throw `cross_error(98)` and catch it INSIDE the library.
/// @return 98 (the caught code) when @p x > 0, else 0. Proves intra-lib unwind.
int  eh_selftest(int x);

/// @brief Throw `cross_error(7)` with NO local catch when @p x > 0.
/// @return 0 when @p x <= 0. Used for the cross-boundary throw->main-catch demo.
int  eh_throw(int x);

/// @brief Construct a Derived, upcast to Base*, `dynamic_cast` back to Derived*.
/// @return 2 on a successful downcast, -1 otherwise. Proves __dynamic_cast +
///         __cxxabiv1 vtables + typeinfo resolve from main. (@p x unused.)
int  eh_dyncast(int x);

#if defined(__cplusplus)
}
#endif

#endif /* CPPEH_H_ */

/**
 * @file ehx-catch.cpp
 * @brief ehx — Plan 6 test 10: a C++ exception thrown in ONE loaded library and
 *        caught in ANOTHER, with the throw propagating through the dl_cross_call
 *        cross-library trampoline frame.
 *
 * Built with exceptions + RTTI ON (mirrors libcppeh's CPPEH_FLAGS), -nostdlib, so the
 * EH/RTTI runtime (__cxa_*, __gxx_personality_v0) is left UND and resolved from main.
 *
 * `eh_throw` is an UND import DEFINED in libcppeh.so -> the loader makes it a cross-lib
 * JUMP_SLOT routed through dl_cross_call. When ehx_catch_cross calls it, eh_throw (in
 * libcppeh) does `throw cross_error(7)`; the unwinder must step:
 *     libcppeh:eh_throw  ->  dl_cross_call (trampoline frame)  ->  ehx:ehx_catch_cross
 * and land in the catch here. This only works because dl_cross_call carries EHABI unwind
 * info (it saves the caller's r10 in a stack slot the .save descriptor can restore) and
 * because cross_error's typeinfo, compiled locally into BOTH libs, matches by NAME (the
 * bare-metal GNU default) -- the same cross-module RTTI unification Plan 5d demonstrated
 * for lib->main, now exercised lib->lib.
 */
#include "cross-error.hpp"
#include <cstdint>

extern "C" {
/** @brief Thrower, defined in libcppeh.so: throws cross_error(7) when x > 0, else returns 0. */
int eh_throw(int x);
}

/// @brief Cross-library throw -> catch. Calls eh_throw (cross-lib, through the
///        trampoline); on the cross_error it returns 1000 + the caught code, so the
///        caller can tell a caught exception (>=1000) from a normal return (0).
/// @return 1000 + e.code() (i.e. 1007) when @p x > 0; 0 if eh_throw returns normally.
extern "C" std::uint32_t ehx_catch_cross(std::uint32_t x) {
    try {
        eh_throw(static_cast<int>(x));    // cross-lib -> libcppeh, which throws
        return 0;                          // (reached only when x == 0)
    } catch (const cross_error& e) {
        return 1000u + e.code();           // unwound back THROUGH dl_cross_call
    }
}

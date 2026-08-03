/**
 * @file cppeh.cpp
 * @brief libcppeh implementation: throw/catch and dynamic_cast across the
 *        module boundary, with the EH/RTTI runtime resolved from main.
 *
 * Built with exceptions + RTTI ON (no -fno-exceptions/-fno-rtti) and -nostdlib,
 * so the EH/RTTI runtime (__cxa_*, __gxx_personality_v0, __dynamic_cast, the
 * __cxxabiv1 typeinfo vtables) is left UND for main to provide. The only new
 * relocation vs. libcppsh is one R_ARM_REL32 in .ARM.extab against the local
 * typeinfo of cross_error (load-bias-invariant: the target is defined here).
 */
#include "cppeh.h"
#include "cross-error.hpp"

namespace {

/// @brief Polymorphic base for the cross-module dynamic_cast demo (who() == 1).
struct Base {
    virtual ~Base() {}
    virtual int who() const { return 1; }   ///< Identity tag; overridden by Derived.
};
/// @brief Derived type the dynamic_cast recovers (who() == 2).
struct Derived : Base {
    int who() const override { return 2; }
};

} // namespace

extern "C" int eh_selftest(int x) {
    try {
        if (x > 0) throw cross_error(98);
        return 0;
    } catch (const cross_error& e) {
        return static_cast<int>(e.code()); // 98
    }
}

extern "C" int eh_throw(int x) {
    if (x > 0) throw cross_error(7);
    return 0;
}

extern "C" int eh_dyncast(int /*unused*/) {
    Derived d;
    Base* b = &d;
    Derived* dd = dynamic_cast<Derived*>(b);
    return dd ? dd->who() : -1; // 2
}

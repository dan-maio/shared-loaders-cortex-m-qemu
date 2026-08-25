#pragma once
#include <cstdint>
#include <cstdio>
#include <span>
#include "diag/trace.h"
#include "fs-archive.hpp"
#include "xip/static/nopic.hpp"
#include "module.h"

/// Load a module from the flat-fs archive (basename lookup).
inline bool xload(const char* name, scl::Module& out) { return out.load_file(name); }

inline bool is_flash(std::uint32_t a) { return (a & 0xFF000000u) == 0x08000000u; }

// ---------------------------------------------------------------------------
// Direct-call helpers — THE POINT of the no-pic model.
//
// The module is linked at fixed addresses (`-fno-pic -mlong-calls`), so
// resolve() returns its plain absolute function address and calling it is an
// ORDINARY C++ indirect call. There is no r10 to set, no got_base to carry, no
// pic_call_* trampoline and no `register void* r10_val` — the code a no-pic
// module runs with is exactly the code a statically linked function runs with.
// ---------------------------------------------------------------------------

/// Call a module function with N args, no r10, no trampoline.
template<typename R, typename... A>
inline R call(const scl::Module& m, const char* name, A... args) {
    auto fn = reinterpret_cast<R (*)(A...)>(m.sym(name));
    return fn(args...);
}

/// Read a module global.
inline std::uint32_t rd(const scl::Module& m, const char* name) {
    void* p = m.sym(name);
    return p ? *reinterpret_cast<volatile std::uint32_t*>(p) : 0;
}

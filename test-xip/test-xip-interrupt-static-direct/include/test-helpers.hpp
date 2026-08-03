#pragma once
#include <cstdint>
#include <cstdio>
#include <span>
#include "diag/trace.h"
#include "fs-archive.hpp"
#include "xip/static/adapter.hpp"
#include "scl/pic-trampoline.h"
#include "module.h"

// Declare the global register variable representing r10 (the GOT base register)
register void* r10_val asm("r10");

inline bool xload(const char* name, scl::Module& out) {
    return out.load_file(name, nullptr, 0, nullptr);
}

inline bool is_flash(std::uint32_t a) { return (a & 0xFF000000u) == 0x08000000u; }

// Direct-call helpers: set r10 and call function in one step
inline void set_r10(const scl::Module& m) {
    r10_val = reinterpret_cast<void*>(m.get_smodule().got_base);
}

inline std::uint32_t call0(const scl::Module& m, void* fn) {
    r10_val = reinterpret_cast<void*>(m.get_smodule().got_base);
    return pic_call_u32(fn, r10_val, 0);
}

inline std::uint32_t call1(const scl::Module& m, void* fn, std::uint32_t a) {
    r10_val = reinterpret_cast<void*>(m.get_smodule().got_base);
    return pic_call_u32(fn, r10_val, a);
}

inline std::uint32_t call2(const scl::Module& m, void* fn, std::uint32_t a, std::uint32_t b) {
    r10_val = reinterpret_cast<void*>(m.get_smodule().got_base);
    return pic_call_u32_2(fn, r10_val, a, b);
}

// Read a global variable from a loaded module
inline std::uint32_t rd(const scl::Module& m, const char* name) {
    void* p = m.sym(name);
    return p ? *reinterpret_cast<volatile std::uint32_t*>(p) : 0;
}

// Read a string pointer from a loaded module
inline const char* rd_str(const scl::Module& m, const char* name) {
    void* p = m.sym(name);
    return p ? *static_cast<const char**>(p) : "(null)";
}

// Call a function by symbol name (0 args)
inline std::uint32_t call_sym0(const scl::Module& m, const char* name) {
    void* fn = m.sym(name);
    if (!fn) { trace_printf("  [err] sym '%s' not found\n", name); return 0; }
    return call0(m, fn);
}

// Call a function by symbol name (1 arg)
inline std::uint32_t call_sym1(const scl::Module& m, const char* name, std::uint32_t a) {
    void* fn = m.sym(name);
    if (!fn) { trace_printf("  [err] sym '%s' not found\n", name); return 0; }
    return call1(m, fn, a);
}

// Call a function by symbol name (2 args)
inline std::uint32_t call_sym2(const scl::Module& m, const char* name, std::uint32_t a, std::uint32_t b) {
    void* fn = m.sym(name);
    if (!fn) { trace_printf("  [err] sym '%s' not found\n", name); return 0; }
    return call2(m, fn, a, b);
}

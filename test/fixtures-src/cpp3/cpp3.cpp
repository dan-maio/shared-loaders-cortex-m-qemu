/**
 * @file cpp3.cpp
 * @brief cpp3 — a C++ loadable library with a real class (Mathy, a method = "CM"),
 *        a global object with a non-trivial constructor (.init_array), a .data var,
 *        a .rodata const, and a .bss counter. See cpp3.h for BUILD.
 *
 * Hard-float math: Mathy::compute calls sinf/cosf/logf/sqrtf — single-precision on
 * the fpv4-sp-d16 FPU — left UND and resolved from main (which links libm and
 * re-exports them). Those lib->main PLT calls pass floats in s0 per the hard-float
 * ABI and need no trampoline (main is -fpic, r10-free). The FM<->cpp3 boundary
 * passes uint32 only, so the existing pic_call_u32* trampolines are unchanged.
 *
 * Cross-lib: cpp3_orchestrate calls c2_relay (-> c2) and cpp3_step calls fm_callback
 * (-> FM); the c2 import is a cross-lib JUMP_SLOT routed through dl_cross_call.
 */
#include "cpp3.h"
#include "fm-iface.h"
#include <cstdint>

extern "C" {
// Hard-float single-precision math, resolved from main (libm).
float sinf(float);
float cosf(float);
float logf(float);
float sqrtf(float);
// Cross-lib imports (defined in c2.so).
uint32_t c2_relay(uint32_t x);
uint32_t c2_step_down(uint32_t d);   // test 14: mutual cross-recursion partner
float    c2_scale(float x);          // test 15: hard-float cross-lib call
// Test 13: C heap, left UND so it binds to MAIN's malloc/free (crt.cpp).
void*    malloc(unsigned int n);
void     free(void* p);
}

namespace cpp3 {
namespace {

/// @brief Library-wide state (lives in .bss; mutated by ctors and methods).
struct Stat {
    std::uint32_t made;   ///< Bumped by the Banner constructor (proves .init_array ran).
    std::uint32_t calls;  ///< Bumped per Mathy instance / method entry (proves own r10/.bss).
};
Stat g_stat;   ///< The single library-wide Stat instance (.bss).

/// @brief Global object with a non-trivial constructor -> one .init_array entry.
struct Banner {
    Banner() noexcept {
        g_stat.made += 0x0300;
        trace_event("cpp3", "Banner.ctor(made)", g_stat.made);
    }
};
Banner g_banner;

/// @brief A real class whose method performs hard-float mathematics.
class Mathy {
public:
    explicit Mathy(std::uint32_t seed) noexcept : seed_(seed) { g_stat.calls += 1; }

    std::uint32_t compute(std::uint32_t x) const noexcept {
        const float t = static_cast<float>(x + seed_) * 0.013f;
        const float v = sinf(t) * 1000.0f      // ┐ all four single-precision libm calls
                      + cosf(t) * 1000.0f      // │ resolve to main and run on the FPU
                      + logf(t + 2.0f) * 100.0f// │
                      + sqrtf(static_cast<float>(x)) * 10.0f; // ┘ (VSQRT.F32)
        const float a = v < 0.0f ? -v : v;
        const std::uint32_t scaled = static_cast<std::uint32_t>(a);
        trace_u32("cpp3.Mathy.compute = ", scaled);
        return scaled;
    }

private:
    std::uint32_t seed_;
};

} // namespace
} // namespace cpp3

/* ---- Exported data (test 6). Declared extern "C" in cpp3.h, so these plain
 *      definitions carry C linkage + external linkage (exported via -Wl,-E). ---- */
const std::uint32_t cpp3_magic = 0x00003333;  // .rodata
std::uint32_t       cpp3_var   = 0x00003030;  // .data

/* Tests 3 & 7: FM ->(CM) cpp3. */
extern "C" std::uint32_t cpp3_method(std::uint32_t seed, std::uint32_t x) {
    trace_event("cpp3", "cpp3_method", x);
    cpp3::Mathy m(seed);            // class instance (touches g_stat -> own r10)
    return m.compute(x);            // the class method (CM)
}

/* Test 6: FM -> cpp3 -> c2 -> cpp3 -> FM. */
extern "C" std::uint32_t cpp3_orchestrate(std::uint32_t x) {
    trace_event("cpp3", "cpp3_orchestrate", x);
    cpp3::g_stat.calls += 1;
    std::uint32_t r = c2_relay(x + cpp3_var);   // cross-lib -> c2 (which calls cpp3_step)
    trace_u32("cpp3: c2_relay -> ", r);
    return r + cpp3_magic;
}

/* Test 6 hop: called cross-lib BY c2, then calls back into FM. */
extern "C" std::uint32_t cpp3_step(std::uint32_t x) {
    trace_event("cpp3", "cpp3_step", x);
    cpp3::g_stat.calls += 1;                     // own .bss (needs cpp3's r10)
    std::uint32_t r = fm_callback(x);            // -> FM (main r10-free)
    trace_u32("cpp3: fm_callback -> ", r);
    return r + cpp3::g_stat.calls;
}

/* Test 14: deep mutual cross-recursion cpp3 <-> c2. Touches own .bss (calls) via own
 * r10 at every level, then descends into c2_step_down. The chain alternates r10 swaps
 * cpp3->c2->cpp3->...; a clean return of sum(0..N) proves r10 integrity at all depths. */
extern "C" std::uint32_t cpp3_recurse(std::uint32_t d) {
    trace_event("cpp3", "cpp3_recurse", d);
    cpp3::g_stat.calls += 1;                     // own .bss (needs cpp3's r10)
    if (d == 0) return 0;
    return d + c2_step_down(d - 1);              // cross-lib -> c2 (r10 swaps to c2)
}

/* Test 15: hard-float passed CROSS-LIB. main->cpp3 boundary stays uint32; the float
 * crosses cpp3 -> c2 internally through dl_cross_call, which must preserve d0-d7 (s0)
 * across its r10 swap. Returns (uint32_t)(x + c2_data); proves VFP transparency. */
extern "C" std::uint32_t cpp3_float_cross(std::uint32_t xi) {
    trace_event("cpp3", "cpp3_float_cross", xi);
    const float x = static_cast<float>(xi);
    const float y = c2_scale(x);                 // cross-lib -> c2 (float in s0)
    const std::uint32_t r = static_cast<std::uint32_t>(y);
    trace_u32("cpp3: c2_scale -> ", r);
    return r;
}

/* Test 13: cross-module HEAP. cpp3 allocates with malloc/free AND new/delete. The
 * library is built -nostdlib -fno-exceptions, so it defines none of these: malloc,
 * free, operator new (_Znwj) and operator delete (_ZdlPv) are left UND and resolve to
 * MAIN's single heap (firmware/src/crt.cpp, a RegionAllocator) through the loader --
 * exactly like libcppsh's operator new. Proves a loaded library shares main's heap.
 * Returns ((sum of n filled words) ^ 0x55); cpp3_heap(8) == 533. */
extern "C" std::uint32_t cpp3_heap(std::uint32_t n) {
    trace_event("cpp3", "cpp3_heap", n);
    if (n == 0u || n > 64u) n = 8u;
    // (1) C heap: malloc/free -> main's allocator.
    auto* arr = static_cast<std::uint32_t*>(malloc(n * sizeof(std::uint32_t)));
    std::uint32_t sum = 0u;
    if (arr) {
        for (std::uint32_t i = 0u; i < n; ++i) arr[i] = (i + 1u) * 0x10u;
        for (std::uint32_t i = 0u; i < n; ++i) sum += arr[i];
        free(arr);
    }
    // (2) C++ heap: operator new/delete -> main's allocator (same heap as malloc).
    //     The asm escape forces a real allocation: without it, -Os elides the new/delete
    //     pair (C++ allocation elision, N3664) and the cross-module operator new is lost.
    auto* obj = new std::uint32_t(sum ^ 0x55u);
    asm volatile("" :: "r"(obj) : "memory");
    const std::uint32_t chk = obj ? *obj : 0u;
    delete obj;
    trace_u32("cpp3: heap sum = ", sum);
    return chk;
}

extern "C" std::uint32_t cpp3_calls(void) { return cpp3::g_stat.calls; }
extern "C" std::uint32_t cpp3_made(void)  { return cpp3::g_stat.made; }

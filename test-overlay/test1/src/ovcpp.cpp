/**
 * @file ovcpp.cpp
 * @brief Overlay module 3 (C++) -- the static counterpart of cpp3.so: a real class
 *        with an out-of-line method, an exported const (ovcpp_magic, cf. cpp3_magic
 *        0x3333) and an exported var (ovcpp_var, cf. cpp3_var). Mirrors the C++ side
 *        of the battery (TESTs 3/6), plus a hard-float variant (ovcpp_fcompute) that
 *        runs the SAME single-precision FPU formula as cpp3.so:cpp3_method.
 *
 * Built non-PIC, -mlong-calls, -fno-exceptions -fno-rtti -fno-use-cxa-atexit, and
 * placed in the shared overlay window. The class ctor/method are ordinary functions
 * in .ovcpp.text: calls AMONG them (ovcpp_method -> Calc::Calc/compute) are intra-
 * window short branches; only the calls OUT to main (trace_*, and sinf/cosf/logf/
 * sqrtf from main's libm) are long_call/BLX. No constructors run automatically for an
 * overlay -- ovcpp_var's initialiser is part of the copied image; no .init_array runs.
 */
#include <cstdint>
#include "fm-iface.h"   /* extern "C" trace_event / trace_u32 (resolved to main) */

extern "C" {
/* Single-precision libm entries, defined in main (which links libm and anchors them
 * via -Wl,-u). The overlay reaches them by long_call (register-indirect BLX), so the
 * FLASH-to-SRAM-window distance is irrelevant -- same provider cpp3.so resolves. */
float sinf(float);
float cosf(float);
float logf(float);
float sqrtf(float);
/* main's heap (crt.cpp), reached by long_call BLX -- same provider cpp3.so resolves
 * its malloc/operator new against. Used by ovcpp_heap (heap comparison, TEST 6). */
void* malloc(unsigned int n);
void  free(void* p);
}

#define OVC_TEXT   __attribute__((section(".ovcpp.text"),   noinline, used))  /**< Place code   in the ovcpp overlay. */
#define OVC_DATA   __attribute__((section(".ovcpp.data"),   used))            /**< Place data   in the ovcpp overlay. */
#define OVC_RODATA __attribute__((section(".ovcpp.rodata"), used))            /**< Place rodata in the ovcpp overlay. */

/* ---- Exported data (C linkage so main references it by plain name). ---- */
/* NB: a namespace-scope `const` defaults to INTERNAL linkage in C++ even inside an
 * extern "C" block (it would link as a mangled, file-local symbol and main would see
 * it undefined -> 0). The explicit `extern` on the const definition forces external
 * linkage so main can read it. The non-const var is external by default. */
extern "C" {
OVC_RODATA extern const std::uint32_t ovcpp_magic = 0x00003333u;   ///< Exported const (== cpp3_magic, 13107).
OVC_DATA          std::uint32_t       ovcpp_var   = 0x00003030u;   ///< Exported .data (== cpp3_var).
}

namespace {
/// @brief A real class with an out-of-line method placed in the overlay window.
class Calc {
public:
    /** @brief Construct with @p seed (its only state). */
    explicit Calc(std::uint32_t seed) noexcept;
    /** @brief Integer compute: seed*x folded with ovcpp_magic. */
    std::uint32_t compute(std::uint32_t x) const noexcept;
private:
    std::uint32_t seed_;   ///< Per-instance seed used by compute().
};

OVC_TEXT Calc::Calc(std::uint32_t seed) noexcept : seed_(seed) {}

/* Integer "math" (no libm): seed*x folded with the exported const. Reading
 * ovcpp_magic here proves the class method reaches the overlay's own .rodata. */
OVC_TEXT std::uint32_t Calc::compute(std::uint32_t x) const noexcept
{
    return seed_ * x + ovcpp_magic;
}
} // namespace

/** @brief The C-callable entry main invokes (battery cpp3_method analogue):
 *         constructs a Calc(seed) in the window and returns Calc::compute(x). */
extern "C" OVC_TEXT std::uint32_t ovcpp_method(std::uint32_t seed, std::uint32_t x)
{
    trace_event("ovcpp", "ovcpp_method", x);     /* -> main (FLASH), long_call */
    Calc m(seed);                                /* class instance, in the window */
    const std::uint32_t r = m.compute(x);        /* class method (intra-window call) */
    trace_u32("ovcpp.Calc.compute = ", r);
    ovcpp_var += 1;
    return r;
}

/**
 * @brief Hard-float computation, byte-for-byte the same formula as cpp3.so:cpp3_method
 *        (Mathy::compute). Single-precision FPU math (fpv4-sp-d16) with all four libm
 *        transcendentals resolved from main -- so the overlay (static, long_call BLX to
 *        libm) and the dynamic lib (relocated JUMP_SLOTs to the same libm) compute an
 *        identical result, the point of the comparison test.
 * @return (uint32_t) of |sinf*1000 + cosf*1000 + logf*100 + sqrtf*10|, a finite value.
 */
extern "C" OVC_TEXT std::uint32_t ovcpp_fcompute(std::uint32_t seed, std::uint32_t x)
{
    trace_event("ovcpp", "ovcpp_fcompute", x);
    const float t = static_cast<float>(x + seed) * 0.013f;
    const float v = sinf(t) * 1000.0f
                  + cosf(t) * 1000.0f
                  + logf(t + 2.0f) * 100.0f
                  + sqrtf(static_cast<float>(x)) * 10.0f;
    const float a = v < 0.0f ? -v : v;
    const std::uint32_t r = static_cast<std::uint32_t>(a);
    trace_u32("ovcpp.fcompute = ", r);
    return r;
}

/**
 * @brief Heap usage from the overlay window, byte-for-byte the same as cpp3.so:cpp3_heap:
 *        malloc/free + operator new/delete, all reaching MAIN's single heap (crt.cpp's
 *        region_allocator). The overlay is non-PIC/-mlong-calls, so each heap call is a
 *        link-time-bound BLX to main -- the static counterpart of the dynamic lib's
 *        relocated JUMP_SLOTs. Both must therefore see the SAME allocator and return the
 *        same checksum, the point of the comparison (TEST 6). ovcpp_heap(8) == 533.
 * @return ((sum of n words, each (i+1)*0x10) ^ 0x55).
 */
extern "C" OVC_TEXT std::uint32_t ovcpp_heap(std::uint32_t n)
{
    trace_event("ovcpp", "ovcpp_heap", n);
    if (n == 0u || n > 64u) n = 8u;
    auto* arr = static_cast<std::uint32_t*>(malloc(n * sizeof(std::uint32_t)));
    std::uint32_t sum = 0u;
    if (arr) {
        for (std::uint32_t i = 0u; i < n; ++i) arr[i] = (i + 1u) * 0x10u;
        for (std::uint32_t i = 0u; i < n; ++i) sum += arr[i];
        free(arr);
    }
    auto* obj = new std::uint32_t(sum ^ 0x55u);   // -> main's operator new (long_call BLX)
    asm volatile("" :: "r"(obj) : "memory");      // keep the alloc (parity with cpp3_heap)
    const std::uint32_t chk = obj ? *obj : 0u;
    delete obj;
    trace_u32("ovcpp.heap sum = ", sum);
    return chk;
}

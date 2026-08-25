/**
 * @file main.cpp
 * @brief XIP/static no-pic firmware — fixed-address modules, plain direct calls.
 *
 * Demonstrates the alternative to the r10 model: fixtures are built
 * `-fno-pic -mlong-calls` and linked at fixed addresses (text @ 0x20018000,
 * data @ 0x2001C000, the `_sxip_*` windows this firmware reserves). Every call
 * from main is an ordinary C++ function-pointer call — no pic_call_*, no
 * got_base, no `register void* r10_val`, no GOT, no PLT.
 */
#include "test-helpers.hpp"
#include "diag/trace.h"
#include "firmware/semihost-file.hpp"
#include "fs-archive.hpp"
#include "scl/image-memory.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <span>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

struct FmInfo { std::uint32_t id; std::uint32_t flags; };

// ---- Symbols the fixtures import from main (resolved at load into RAM) -----
extern "C" {
unsigned int HOST_K = 1000;
unsigned int host_f(unsigned int x) { return x * 2 + 1; }
unsigned int AA = 42;
unsigned int BB = 84;
char*        s1 = const_cast<char*>("main-s1");
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg); }
void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v); }
std::uint32_t fm_var = 123;
struct FmInfo   fm_info = {0x00F1, 0x0007};
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
}

// Anchor the borrowed libc set so printf / puts land in main's .dynsym and the
// loader's import resolution can find them.
#include "link-anchor.hpp"

namespace {

struct SInfo { const char* tag; unsigned int x; unsigned int y; };

bool provision(const char* name) {
    char path[160];
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < sizeof(path); ++d) path[i++] = *d;
    if (i + 1 < sizeof(path)) path[i++] = '/';
    for (const char* n = name; *n && i + 1 < sizeof(path); ++n) path[i++] = *n;
    path[i] = '\0';

    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_printf("  [prov] absent: %s\n", name); return false; }
    const std::size_t sz = src.size();
    auto memp = fw::image_arena().alloc(sz);
    if (!memp) { trace_printf("  [prov] OOM: %s\n", name); return false; }
    auto* buf = static_cast<std::byte*>(*memp);
    bool ok = false;
    if (src.read_at(0, std::span<std::byte>{buf, sz}).has_value()) {
        // NOTE: no import binding here. The no-pic loader patches the R_ARM_ABS32
        // import slots into the RAM windows at LOAD time (nothing to pre-bind).
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    if (!ok) trace_printf("  [prov] fs_add FAILED: %s\n", name);
    return ok;
}

void test_nslib() {
    trace_printf("\n[test] ==== nslib.so (no-pic, fixed-address, plain calls) ====\n");
    scl::Module m;
    if (!xload("nslib.so", m)) { trace_printf("  load FAILED\n"); return; }
    const auto& nm = m.get();
    trace_printf("  text window = 0x%08x  data window = 0x%08x (fixed VAs)\n",
                 static_cast<unsigned>(nm.text_base), static_cast<unsigned>(nm.data_base));

    // Plain function pointers — no r10, no got_base, no trampoline.
    auto s_add      = reinterpret_cast<unsigned (*)(unsigned, unsigned)>(m.sym("s_add"));
    auto s_scale    = reinterpret_cast<unsigned (*)(unsigned)>(m.sym("s_scale"));
    auto s_via      = reinterpret_cast<unsigned (*)(unsigned)>(m.sym("s_via_helper"));
    auto s_apply    = reinterpret_cast<unsigned (*)(unsigned)>(m.sym("s_apply"));
    auto s_text     = reinterpret_cast<const char* (*)()>(m.sym("s_text"));
    auto s_tag      = reinterpret_cast<const char* (*)()>(m.sym("s_tag"));
    auto s_get_a    = reinterpret_cast<unsigned (*)()>(m.sym("s_get_a"));
    auto s_get_b    = reinterpret_cast<unsigned (*)()>(m.sym("s_get_b"));
    auto s_use_host = reinterpret_cast<unsigned (*)(unsigned)>(m.sym("s_use_host"));
    auto s_mix      = reinterpret_cast<unsigned (*)(unsigned)>(m.sym("s_mix"));
    auto s_print    = reinterpret_cast<void (*)()>(m.sym("s_print"));

    // globals + constructor effect (s_a 10->15, s_b=30 in .bss)
    trace_printf("  s_a / s_b (ctor)      = %u / %u\n", rd(m, "s_a"), rd(m, "s_b"));
    trace_printf("  s_add(10, 5)          = %u\n", s_add(10, 5));
    trace_printf("  s_scale(3)            = %u\n", s_scale(3));
    trace_printf("  s_via_helper(5)       = %u\n", s_via(5));
    trace_printf("  s_apply(3)            = %u\n", s_apply(3));
    trace_printf("  s_get_a()             = %u\n", s_get_a());
    // strings via absolute (baked) literals
    trace_printf("  s_text()              = \"%s\"\n", s_text());
    trace_printf("  s_tag()               = \"%s\"\n", s_tag());
    // struct global s_info (read through the data window)
    const auto* s_info = static_cast<const SInfo*>(m.sym("s_info"));
    trace_printf("  s_info.tag            = \"%s\"\n", s_info ? s_info->tag : "(null)");
    trace_printf("  s_info.x / s_info.y   = %u / %u\n",
                 s_info ? s_info->x : 0, s_info ? s_info->y : 0);
    // imports from main: DATA (HOST_K) + FUNCTION (host_f)
    trace_printf("  s_use_host(5)         = %u   (host_f(5)+HOST_K)\n", s_use_host(5));
    // FUNCTION import printf, called from the text window
    trace_printf("  s_print():\n");
    s_print();
    // mutating call LAST: s_mix(4) sets s_b using host_f
    trace_printf("  s_mix(4)              = %u\n", s_mix(4));
    trace_printf("  s_get_b() (after mix) = %u\n", s_get_b());

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

void test_nmylib1() {
    trace_printf("\n[test] ==== nmylib1.so (no-pic) + reload proof ====\n");
    for (int cycle = 0; cycle < 2; ++cycle) {
        scl::Module m;
        if (!xload("nmylib1.so", m)) { trace_printf("  load FAILED\n"); return; }
        trace_printf("  cycle %d: aa/bb (ctor) = %u/%u, "
                     "library_function(39)=%u, library_function2(10)=%u, "
                     "library_function3(10)=%u\n",
                     cycle, rd(m, "aa"), rd(m, "bb"),
                     call<unsigned>(m, "library_function", 39u),
                     call<unsigned>(m, "library_function2", 10u),
                     call<unsigned>(m, "library_function3", 10u));
        m = scl::Module{};
    }
    trace_printf("  reload produced identical results (clean data window)\n");
}

void test_cross() {
    trace_printf("\n[test] ==== nprov.so + ncons.so (cross-module, no trampoline) ====\n");
    scl::Module mp, mc;
    if (!xload("nprov.so", mp)) { trace_printf("  load nprov FAILED\n"); return; }
    auto p_fn = reinterpret_cast<unsigned (*)(unsigned)>(mp.sym("p_fn"));
    trace_printf("  nprov: p_fn(10)         = %u\n", p_fn(10));       // 3*10+7 = 37

    if (!xload("ncons.so", mc)) { trace_printf("  load ncons FAILED\n"); return; }
    auto c_fn     = reinterpret_cast<unsigned (*)(unsigned)>(mc.sym("c_fn"));
    auto c_double = reinterpret_cast<unsigned (*)(unsigned)>(mc.sym("c_double"));
    trace_printf("  ncons: c_fn(10)         = %u   (p_fn(10)+1)\n", c_fn(10));       // 38
    trace_printf("  ncons: c_double(10)     = %u   (p_fn(p_fn(10)))\n", c_double(10)); // 118
    trace_printf("  cross-module call ran through a plain indirect call\n");
}

}  // namespace

extern "C" int main(void) {
    trace_printf("\nscl-fw-xip-static-no-pic: fixed-address static loader, NO r10\n");

    trace_printf("[prov] formatting flat-fs\n");
    if (!fw::fs_format()) trace_printf("[prov] fs_format FAILED\n");
    provision("nslib.so");
    provision("nmylib1.so");
    provision("nprov.so");
    provision("ncons.so");
    trace_printf("[boot] flat-fs files = %u\n", fw::fs_count());

    test_nslib();
    test_nmylib1();
    test_cross();

    trace_printf("\nscl-fw-xip-static-no-pic: done\n");
    std::exit(0);
    for (;;) { }
}

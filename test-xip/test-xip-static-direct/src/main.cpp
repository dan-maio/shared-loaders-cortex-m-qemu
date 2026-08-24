/**
 * @file main.cpp
 * @brief XIP firmware (hosted platform) — STATIC loader flavour - Direct calls variant.
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

// ---- Symbols that loaded libraries import from main ----------
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

// Anchor the borrowed libc/libsupc++/libm set so the loaded libraries' imports
// (__dynamic_cast, printf, puts, operator new, sinf, ...) land in main's .dynsym
// and bind_host_imports can pre-link them directly. Same set as the semihosting
// flavour; without it the trampoline cannot resolve e.g. __dynamic_cast (RTTI).
#include "link-anchor.hpp"

// ---- Struct definitions for library globals (needed for member access) ----
extern "C" {
struct SInfo { const char* tag; unsigned int x; unsigned int y; };
extern struct SInfo s_info;

struct SLib { const char* a; float f; const char* bb; unsigned int b; unsigned int c; };
extern struct SLib s_lib;
}

namespace {

int g_bound = -99;
int g_patch1 = -99;

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
        int patched = xip::stat::bind_host_imports(std::span<std::byte>{buf, sz});
        if (std::strcmp(name, "slib.so") == 0) g_bound = patched;
        if (std::strcmp(name, "mylib1.so") == 0) g_patch1 = patched;
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    return ok;
}

void test_slib() {
    trace_printf("\n[test] ==== slib.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("slib.so", m)) { trace_printf("  load FAILED\n"); return; }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");
    trace_printf("  got_base              = 0x%08x (RAM)\n", static_cast<std::uint32_t>(sm.got_base));
    trace_printf("  [bind] fn imports bound at provision = %d\n", g_bound);

    // globals + constructor effect (s_a 10->15, s_b=30 in .bss)
    trace_printf("  s_a                   = %u\n", rd(m, "s_a"));
    trace_printf("  s_b                   = %u\n", rd(m, "s_b"));
    // pure functions from flash (direct calls)
    trace_printf("  s_add(10, 5)          = %u\n", call_sym2(m, "s_add", 10, 5));
    trace_printf("  s_scale(3)            = %u\n", call_sym1(m, "s_scale", 3));
    trace_printf("  s_via_helper(5)       = %u\n", call_sym1(m, "s_via_helper", 5));
    trace_printf("  s_apply(3)            = %u\n", call_sym1(m, "s_apply", 3));
    trace_printf("  s_get_a()             = %u\n", call_sym0(m, "s_get_a"));
    // strings via R_ARM_RELATIVE
    trace_printf("  s_text()              = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "s_text")));
    trace_printf("  s_tag()               = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "s_tag")));
    // struct global s_info
    trace_printf("  s_info.tag            = \"%s\"\n", s_info.tag);
    trace_printf("  s_info.x / s_info.y   = %u / %u\n", s_info.x, s_info.y);
    // imports from main: DATA (HOST_K) + FUNCTION (host_f)
    trace_printf("  s_use_host(5)         = %u   (host_f(5)+HOST_K)\n", call_sym1(m, "s_use_host", 5));
    // FUNCTION import printf, called from flash
    trace_printf("  s_print():\n");
    call_sym0(m, "s_print");
    // mutating call LAST: s_mix(4) sets s_b using host_f
    trace_printf("  s_mix(4)              = %u\n", call_sym1(m, "s_mix", 4));
    trace_printf("  s_get_b() (after mix) = %u\n", call_sym0(m, "s_get_b"));

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

void test_mylib1() {
    trace_printf("\n[test] ==== mylib1.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("mylib1.so", m)) { trace_printf("  load FAILED\n"); return; }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", rd(m, "aa"));
    trace_printf("  bb                    = %u\n", rd(m, "bb"));
    // scalar functions (pure)
    trace_printf("  library_function(39)  = %u\n", call_sym1(m, "library_function", 39));
    trace_printf("  library_function2(10) = %u\n", call_sym1(m, "library_function2", 10));
    trace_printf("  g(5)                  = %u\n", call_sym1(m, "g", 5));
    trace_printf("  gg(5)                 = %u\n", call_sym1(m, "gg", 5));
    trace_printf("  fff(5)                = %u\n", call_sym1(m, "fff", 5));
    trace_printf("  callback_f(10)        = %u\n", call_sym1(m, "callback_f", 10));
    trace_printf("  foo()                 = %u\n", call_sym0(m, "foo"));
    trace_printf("  boo()                 = %u\n", call_sym0(m, "boo"));
    trace_printf("  boo2()                = %u\n", call_sym0(m, "boo2"));
    trace_printf("  lkj()                 = %u\n", call_sym0(m, "lkj"));
    // strings via R_ARM_RELATIVE
    trace_printf("  retstr()              = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "retstr")));
    trace_printf("  retstr_()             = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "retstr_")));
    // struct global s_lib
    trace_printf("  s_lib.a               = \"%s\"\n", s_lib.a);
    trace_printf("  s_lib.bb              = \"%s\"\n", s_lib.bb);
    trace_printf("  s_lib.b / s_lib.c     = %u / %u\n", s_lib.b, s_lib.c);
    // void functions
    call_sym0(m, "print_lib");
    call_sym1(m, "print_lib2", 0);
    // mutating call LAST
    trace_printf("  library_function3(10) = %u\n", call_sym1(m, "library_function3", 10));
    trace_printf("  bb (after lib3)       = %u\n", rd(m, "bb"));

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("mylib2.so", m)) { trace_printf("  load FAILED\n"); return; }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");

    trace_printf("  aaa                   = %u\n", rd(m, "aaa"));
    trace_printf("  bbb                   = %u\n", rd(m, "bbb"));
    trace_printf("  xx                    = %u\n", rd(m, "xx"));
    trace_printf("  lf(39)                = %u\n", call_sym1(m, "lf", 39));
    trace_printf("  lf2(10)               = %u\n", call_sym1(m, "lf2", 10));
    trace_printf("  xxx()                 = %u\n", call_sym0(m, "xxx"));
    trace_printf("  mm()                  = %u\n", call_sym0(m, "mm"));
    trace_printf("  str2                  = \"%s\"\n", rd_str(m, "str2"));
    trace_printf("  lf3(10)               = %u\n", call_sym1(m, "lf3", 10));
    trace_printf("  bbb (after lf3)       = %u\n", rd(m, "bbb"));
    call_sym0(m, "pl");

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

}  // namespace

void test_c1_c2_cpp3();
void test_ehx_cons_ppeh_ppsh_prov();
void xip_walkthrough();
void xip_walkthrough_complex();

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace_printf("\nscl-fw-xip-static-direct: full-surface XIP test, STATIC loader direct calls\n");

    trace_printf("[prov] formatting flat-fs\n");
    if (!fw::fs_format()) trace_printf("[prov] fs_format FAILED\n");
    provision("slib.so");
    provision("mylib1.so");
    provision("mylib2.so");
    provision("c1.so");
    provision("c2.so");
    provision("cpp3.so");
    provision("libprov.so");
    provision("libcons.so");
    provision("libcppeh.so");
    provision("libcppsh.so");
    provision("ehx_catch.so");
    trace_printf("[boot] flat-fs files = %u\n", fw::fs_count());

    test_slib();
    test_mylib1();
    test_mylib2();
    test_c1_c2_cpp3();
    test_ehx_cons_ppeh_ppsh_prov();
    xip_walkthrough();
    xip_walkthrough_complex();

    trace_printf("\nscl-fw-xip-static-direct: done\n");
    std::exit(0);
    for (;;) { }
}

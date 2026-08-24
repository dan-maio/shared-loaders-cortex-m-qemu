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

// ---- ABI the library imports from main (a static symbol registry) ----------
extern "C" {
__attribute__((used)) unsigned int HOST_K = 1000;                 // data import
__attribute__((used)) unsigned int host_f(unsigned int x) { return x * 2 + 1; }  // fn import
__attribute__((used)) unsigned int AA = 42;
__attribute__((used)) unsigned int BB = 84;
__attribute__((used)) char*        s1 = const_cast<char*>("main-s1");
__attribute__((used)) unsigned int f(unsigned int x) { return x + 10; }
__attribute__((used)) unsigned int w(unsigned int x) { return x + 20; }
__attribute__((used)) void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg); }
__attribute__((used)) void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v); }
__attribute__((used)) std::uint32_t fm_var = 123;
__attribute__((used)) struct FmInfo   fm_info = {0x00F1, 0x0007};
__attribute__((used)) std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
}

#include "link-anchor.hpp"   // anchors the borrowed libc/libsupc++/libm set

extern "C" {
// slib
extern unsigned int s_a;
extern unsigned int s_b;
extern const char*  s_msg;
struct SInfo { const char* tag; unsigned int x; unsigned int y; };
extern struct SInfo s_info;
unsigned int s_add(unsigned int, unsigned int);
unsigned int s_scale(unsigned int);
unsigned int s_via_helper(unsigned int);
const char*  s_text();
const char*  s_tag();
unsigned int s_get_a();
unsigned int s_get_b();
unsigned int s_apply(unsigned int);
unsigned int s_use_host(unsigned int);
unsigned int s_mix(unsigned int);
void         s_print();

// mylib1
extern unsigned int aa;
extern unsigned int bb;
struct SLib { const char* a; float f; const char* bb; unsigned int b; unsigned int c; };
extern struct SLib s_lib;
unsigned int library_function(unsigned int);
unsigned int library_function2(unsigned int);
unsigned int library_function3(unsigned int);
unsigned int g(unsigned int);
unsigned int gg(unsigned int);
unsigned int fff(unsigned int);
unsigned int callback_f(unsigned int);
unsigned int foo();
unsigned int boo();
unsigned int boo2();
unsigned int lkj();
const char*  retstr();
const char*  retstr_();
void         print_lib();
void         print_lib2(unsigned int);

// mylib2
extern unsigned int aaa;
extern unsigned int bbb;
extern unsigned int xx;
extern const char*  str2;
unsigned int lf(unsigned int);
unsigned int lf2(unsigned int);
unsigned int lf3(unsigned int);
unsigned int xxx();
unsigned int mm();
void         pl();
void         pl2(char*);
}

namespace {

int g_bound = -99;
int g_patch1 = -99;

void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    for (const char* n = name; *n && i + 1 < cap; ++n) out[i++] = *n;
    out[i] = '\0';
}

bool provision(const char* name) {
    char path[160];
    make_path(path, sizeof(path), name);
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
    if (!xload("slib.so", m)) {
        trace_printf("  load FAILED\n"); return;
    }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");
    trace_printf("  got_base              = 0x%08x (RAM)\n", static_cast<std::uint32_t>(sm.got_base));
    trace_printf("  [bind] fn imports bound at provision = %d\n", g_bound);

    // Patch global register R10 so direct calls resolve GOT references correctly
    r10_val = reinterpret_cast<void*>(sm.got_base);

    // globals + constructor effect (s_a 10->15, s_b=30 in .bss)
    trace_printf("  s_a                   = %u\n", s_a);
    trace_printf("  s_b                   = %u\n", s_b);
    // pure functions from flash (direct calls)
    trace_printf("  s_add(10, 5)          = %u\n", s_add(10, 5));
    trace_printf("  s_scale(3)            = %u\n", s_scale(3));
    trace_printf("  s_via_helper(5)       = %u\n", s_via_helper(5));
    trace_printf("  s_apply(3)            = %u\n", s_apply(3));
    trace_printf("  s_get_a()             = %u\n", s_get_a());
    // strings via R_ARM_RELATIVE
    trace_printf("  s_text()              = \"%s\"\n", s_text());
    trace_printf("  s_tag()               = \"%s\"\n", s_tag());
    // struct global s_info
    trace_printf("  s_info.tag            = \"%s\"\n", s_info.tag);
    trace_printf("  s_info.x / s_info.y   = %u / %u\n", s_info.x, s_info.y);
    // imports from main: DATA (HOST_K) + FUNCTION (host_f)
    trace_printf("  s_use_host(5)         = %u   (host_f(5)+HOST_K)\n", s_use_host(5));
    // FUNCTION import printf, called from flash
    trace_printf("  s_print():\n");
    s_print();
    // mutating call LAST: s_mix(4) sets s_b using host_f
    trace_printf("  s_mix(4)              = %u\n", s_mix(4));
    trace_printf("  s_get_b() (after mix) = %u\n", s_get_b());

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

void test_mylib1() {
    trace_printf("\n[test] ==== mylib1.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("mylib1.so", m)) {
        trace_printf("  load FAILED\n"); return;
    }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // Set register R10 for mylib1
    r10_val = reinterpret_cast<void*>(sm.got_base);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", aa);
    trace_printf("  bb                    = %u\n", bb);
    // scalar functions (pure)
    trace_printf("  library_function(39)  = %u\n", library_function(39));
    trace_printf("  library_function2(10) = %u\n", library_function2(10));
    trace_printf("  g(5)                  = %u\n", g(5));
    trace_printf("  gg(5)                 = %u\n", gg(5));
    trace_printf("  fff(5)                = %u\n", fff(5));
    trace_printf("  callback_f(10)        = %u\n", callback_f(10));
    trace_printf("  foo()                 = %u\n", foo());
    trace_printf("  boo()                 = %u\n", boo());
    trace_printf("  boo2()                = %u\n", boo2());
    trace_printf("  lkj()                 = %u\n", lkj());
    // strings via R_ARM_RELATIVE
    trace_printf("  retstr()              = \"%s\"\n", retstr());
    trace_printf("  retstr_()             = \"%s\"\n", retstr_());
    // struct global s_lib
    trace_printf("  s_lib.a               = \"%s\"\n", s_lib.a);
    trace_printf("  s_lib.bb              = \"%s\"\n", s_lib.bb);
    trace_printf("  s_lib.b / s_lib.c     = %u / %u\n", s_lib.b, s_lib.c);
    // void functions
    print_lib();  print_lib2(0);
    // mutating call LAST
    trace_printf("  library_function3(10) = %u\n", library_function3(10));
    trace_printf("  bb (after lib3)       = %u\n", bb);

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("mylib2.so", m)) {
        trace_printf("  load FAILED\n"); return;
    }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");

    // Set register R10 for mylib2
    r10_val = reinterpret_cast<void*>(sm.got_base);

    trace_printf("  aaa                   = %u\n", aaa);
    trace_printf("  bbb                   = %u\n", bbb);
    trace_printf("  xx                    = %u\n", xx);
    trace_printf("  lf(39)                = %u\n", lf(39));
    trace_printf("  lf2(10)               = %u\n", lf2(10));
    trace_printf("  xxx()                 = %u\n", xxx());
    trace_printf("  mm()                  = %u\n", mm());
    trace_printf("  str2                  = \"%s\"\n", str2);
    trace_printf("  lf3(10)               = %u\n", lf3(10));
    trace_printf("  bbb (after lf3)       = %u\n", bbb);
    pl();

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

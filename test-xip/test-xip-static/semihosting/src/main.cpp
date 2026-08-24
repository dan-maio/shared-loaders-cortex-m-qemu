/**
 * @file main.cpp
 * @brief XIP firmware (hosted platform) — STATIC loader flavour.
 *
 * Mirrors test-dynamic-nolib/test1, but with no dynamic loader: there is no
 * dlopen/dlsym and no call-time binding. The library is resolved once, at load,
 * and its GOT is patched in place (classic static-linker model). The R-X segment
 * still executes in place FROM FLASH; only the RW segment is copied to RAM.
 *
 * Cross-library function imports are routed through the shared xip_cross_call
 * trampoline: an import main cannot provide is pre-linked to the trampoline at
 * provision time, and at call time it resolves the symbol against the currently
 * loaded modules (registered by load(), dropped by unload()) and swaps r10 to
 * the callee's got_base. Dependencies must therefore be loaded first.
 *
 * Full surface, print-only (like the classic tests): every exported function,
 * global, constructor effect, string literal (R_ARM_RELATIVE), struct member,
 * function-pointer path, a DATA import from main (HOST_K) and FUNCTION imports
 * from main (host_f, printf).
 */
#include "diag/trace.h"
#include "firmware/semihost-file.hpp"
#include "fs-archive.hpp"
#include "scl/image-memory.hpp"
#include "scl/pic-trampoline.h"
#include "xip/static/adapter.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
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

// Resolution is 100% against main's OWN .dynsym (fw::link_context::from_self(),
// inside the loader). main is built -pie -Wl,-E, so every symbol main *defines*
// (above) lands in .dynsym automatically — no host table, no -u for those.
//
#include "link-anchor.hpp"   // anchors the borrowed libc/libsupc++/libm set

namespace {

int g_bound = -99;   // #function-imports bound at provision (printed in the test)
int g_patch1 = -99;

void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    for (const char* n = name; *n && i + 1 < cap; ++n) out[i++] = *n;
    out[i] = '\0';
}
bool is_flash(std::uint32_t a) { return (a & 0xFF000000u) == 0x08000000u; }

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
        // Resolve external FUNCTION imports into the flash-bound image now, so the
        // R-X PLT reaches main directly once executing in place (main is r10-free).
        int patched = xip::stat::bind_host_imports(std::span<std::byte>{buf, sz});
        if (std::strcmp(name, "slib.so") == 0) g_bound = patched;
        if (std::strcmp(name, "mylib1.so") == 0) g_patch1 = patched;
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    if (!ok) trace_printf("  [prov] FAILED %s\n", name);
    return ok;
}

// call/read helpers (enter the library with r10 = got_base).
std::uint32_t C0(xip::stat::SModule& m, const char* s) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), 0) : 0; }
std::uint32_t C1(xip::stat::SModule& m, const char* s, std::uint32_t a) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), a) : 0; }
std::uint32_t C2(xip::stat::SModule& m, const char* s, std::uint32_t a, std::uint32_t b) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32_2(fn, reinterpret_cast<void*>(m.got_base), a, b) : 0; }
std::uint32_t RD(xip::stat::SModule& m, const char* s) {
    void* p = xip::stat::resolve(m, s); return p ? *reinterpret_cast<volatile std::uint32_t*>(p) : 0; }
const char* CSTR(xip::stat::SModule& m, const char* s) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? reinterpret_cast<const char*>(pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), 0)) : "(null)"; }

void test_slib() {
    trace_printf("\n[test] ==== slib.so (static XIP) ====\n");
    const fw::FsEntry* e = fw::fs_find("slib.so");
    if (!e) { trace_printf("  not provisioned\n"); return; }

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m)) {
        trace_printf("  load FAILED\n"); return;
    }
    void* sadd = xip::stat::resolve(m, "s_add");
    trace_printf("  loaded: %s;  s_add @ 0x%08x (%s)\n",
                 m.ok ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(sadd),
                 is_flash(reinterpret_cast<std::uint32_t>(sadd)) ? "FLASH" : "RAM");
    trace_printf("  got_base              = 0x%08x (RAM)\n", static_cast<std::uint32_t>(m.got_base));
    trace_printf("  [bind] fn imports bound at provision = %d\n", g_bound);

    // globals + constructor effect (s_a 10->15, s_b=30 in .bss)
    trace_printf("  s_a                   = %u\n", RD(m, "s_a"));
    trace_printf("  s_b                   = %u\n", RD(m, "s_b"));
    // pure functions from flash
    trace_printf("  s_add(10, 5)          = %u\n", C2(m, "s_add", 10, 5));
    trace_printf("  s_scale(3)            = %u\n", C1(m, "s_scale", 3));
    trace_printf("  s_via_helper(5)       = %u\n", C1(m, "s_via_helper", 5));
    trace_printf("  s_apply(3)            = %u\n", C1(m, "s_apply", 3));   // fn pointer
    trace_printf("  s_get_a()             = %u\n", C0(m, "s_get_a"));
    // strings via R_ARM_RELATIVE (literal in flash .rodata; pointer relocated)
    trace_printf("  s_text()              = \"%s\"\n", CSTR(m, "s_text"));
    trace_printf("  s_tag()               = \"%s\"\n", CSTR(m, "s_tag"));
    // struct global s_info { char* tag; uint x; uint y }
    if (void* p = xip::stat::resolve(m, "s_info")) {
        auto* b = reinterpret_cast<std::uint8_t*>(p);
        trace_printf("  s_info.tag            = \"%s\"\n", *reinterpret_cast<char**>(b + 0));
        trace_printf("  s_info.x / s_info.y   = %u / %u\n",
                     *reinterpret_cast<std::uint32_t*>(b + 4), *reinterpret_cast<std::uint32_t*>(b + 8));
    }
    // imports from main: DATA (HOST_K) + FUNCTION (host_f)
    trace_printf("  s_use_host(5)         = %u   (host_f(5)+HOST_K)\n", C1(m, "s_use_host", 5));
    // FUNCTION import printf, called from flash
    trace_printf("  s_print():\n");
    C0(m, "s_print");
    // mutating call LAST: s_mix(4) sets s_b using host_f
    trace_printf("  s_mix(4)              = %u\n", C1(m, "s_mix", 4));
    trace_printf("  s_get_b() (after mix) = %u\n", C0(m, "s_get_b"));

    xip::stat::unload(m);
    trace_printf("  unload ok\n");
}

void test_mylib1() {
    trace_printf("\n[test] ==== mylib1.so (static XIP) ====\n");
    const fw::FsEntry* e = fw::fs_find("mylib1.so");
    if (!e) { trace_printf("  not provisioned\n"); return; }

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m)) {
        trace_printf("  load FAILED\n"); return;
    }
    void* lf = xip::stat::resolve(m, "library_function");
    trace_printf("  loaded: %s;  library_function @ 0x%08x (%s)\n",
                 m.ok ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", RD(m, "aa"));
    trace_printf("  bb                    = %u\n", RD(m, "bb"));
    // scalar functions (pure — before the bb-mutating library_function3)
    trace_printf("  library_function(39)  = %u\n", C1(m, "library_function", 39));
    trace_printf("  library_function2(10) = %u\n", C1(m, "library_function2", 10));
    trace_printf("  g(5)                  = %u\n", C1(m, "g", 5));
    trace_printf("  gg(5)                 = %u\n", C1(m, "gg", 5));
    trace_printf("  fff(5)                = %u\n", C1(m, "fff", 5));
    trace_printf("  callback_f(10)        = %u\n", C1(m, "callback_f", 10));
    trace_printf("  foo()                 = %u\n", C0(m, "foo"));
    trace_printf("  boo()                 = %u\n", C0(m, "boo"));
    trace_printf("  boo2()                = %u\n", C0(m, "boo2"));
    trace_printf("  lkj()                 = %u\n", C0(m, "lkj"));
    // strings via R_ARM_RELATIVE (literal in flash .rodata; pointer relocated)
    trace_printf("  retstr()              = \"%s\"\n", CSTR(m, "retstr"));
    trace_printf("  retstr_()             = \"%s\"\n", CSTR(m, "retstr_"));
    // struct global s_lib { char*a; float f; char*bb; uint b; uint c }
    if (void* p = xip::stat::resolve(m, "s_lib")) {
        auto* b = reinterpret_cast<std::uint8_t*>(p);
        trace_printf("  s_lib.a               = \"%s\"\n", *reinterpret_cast<char**>(b + 0));
        trace_printf("  s_lib.bb              = \"%s\"\n", *reinterpret_cast<char**>(b + 8));
        trace_printf("  s_lib.b / s_lib.c     = %u / %u\n",
                     *reinterpret_cast<std::uint32_t*>(b + 12), *reinterpret_cast<std::uint32_t*>(b + 16));
    }
    // void functions (side-effects on statics; just exercise the code path)
    C0(m, "print_lib");  C1(m, "print_lib2", 0);
    // mutating call LAST: library_function3(10) sets bb, uses main's w()
    trace_printf("  library_function3(10) = %u\n", C1(m, "library_function3", 10));
    trace_printf("  bb (after lib3)       = %u\n", RD(m, "bb"));

    xip::stat::unload(m);
    trace_printf("  unload ok\n");
}

void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so (static XIP) ====\n");
    const fw::FsEntry* e = fw::fs_find("mylib2.so");
    if (!e) { trace_printf("  not provisioned\n"); return; }

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m)) {
        trace_printf("  load FAILED\n"); return;
    }
    void* lf = xip::stat::resolve(m, "lf");
    trace_printf("  loaded: %s;  lf @ 0x%08x (%s)\n",
                 m.ok ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");

    trace_printf("  aaa                   = %u\n", RD(m, "aaa"));
    trace_printf("  bbb                   = %u\n", RD(m, "bbb"));
    trace_printf("  xx                    = %u\n", RD(m, "xx"));
    trace_printf("  lf(39)                = %u\n", C1(m, "lf", 39));
    trace_printf("  lf2(10)               = %u\n", C1(m, "lf2", 10));
    trace_printf("  xxx()                 = %u\n", C0(m, "xxx"));   // uses main's printf + BB/AA
    trace_printf("  mm()                  = %u\n", C0(m, "mm"));
    if (void* p = xip::stat::resolve(m, "str2"))
        trace_printf("  str2                  = \"%s\"\n", *reinterpret_cast<char**>(p));
    trace_printf("  lf3(10)               = %u\n", C1(m, "lf3", 10));   // uses main's w()
    trace_printf("  bbb (after lf3)       = %u\n", RD(m, "bbb"));
    C0(m, "pl");                 // void (printf + bbb-=10)

    xip::stat::unload(m);
    trace_printf("  unload ok\n");
}

}  // namespace

void test_c1_c2_cpp3();
void test_ehx_cons_ppeh_ppsh_prov();
// Declared in walkthrough.cpp: didactic flash-vs-RAM execution-address map.
void xip_walkthrough();
// Declared in walkthrough_complex.cpp: complex static walkthrough.
void xip_walkthrough_complex();

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace_printf("\nscl-fw-xip-static: full-surface XIP test, STATIC loader (code from flash)\n");

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

    trace_printf("\nscl-fw-xip-static: done\n");
    std::exit(0);
    for (;;) { }
}


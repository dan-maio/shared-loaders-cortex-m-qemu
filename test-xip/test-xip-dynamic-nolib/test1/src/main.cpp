/**
 * @file main.cpp
 * @brief XIP firmware (hosted platform): provision the flat-fs, XIP-load each
 *        library, and exercise its surface FROM FLASH — every exported function,
 *        global, constructor effect, string literal (R_ARM_RELATIVE), struct
 *        member, function-pointer path, weak symbol, and main-resolved import.
 *        Also exercises copy-whole (fused RWX) libraries with cross-lib calls
 *        (c1, c2, cpp3).
 *
 * Hosted start phase (weact-f411ce-hosted): main runs its constructors, has a
 * real heap, and links libc/libm/libstdc++ so library imports (printf/f/w/AA)
 * resolve against a real runtime in main (-pie -Wl,-E). Fault handlers dump regs.
 */
#include "test-helpers.hpp"
#include "firmware/semihost-file.hpp"
#include "image-memory.hpp"
#include "xip/relocate.hpp"
#include "dynamic-nolib/dl.hpp"
#include <cstdio>
#include <cstring>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

// ---- ABI that the libraries import from main (exported via -pie -Wl,-E) --------
// Symbols main DEFINES land in .dynsym on -E alone (no -u needed). The loader
// resolves every library import against this .dynsym (link_context::from_self).
struct FmInfo { std::uint32_t id; std::uint32_t flags; };
extern "C" {
__attribute__((used)) unsigned int AA = 42;
__attribute__((used)) unsigned int BB = 84;
__attribute__((used)) char*        s1 = const_cast<char*>("main-s1");
__attribute__((used)) unsigned int f(unsigned int x) { return x + 10; }
__attribute__((used)) unsigned int w(unsigned int x) { return x + 20; }
// Trace / FM exports consumed by c1, c2, cpp3 (same pattern as nolib test2/3/4).
__attribute__((used)) void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg); }
__attribute__((used)) void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v); }
__attribute__((used)) std::uint32_t fm_var = 123;
__attribute__((used)) struct FmInfo   fm_info = {0x00F1, 0x0007};
__attribute__((used)) std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
}

#include "link-anchor.hpp"   // anchors the borrowed libc/libsupc++/libm set

namespace {

int g_patch1 = -99;   // patch_main_imports result for mylib1 (printed in the test)

void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    for (const char* n = name; *n && i + 1 < cap; ++n) out[i++] = *n;
    out[i] = '\0';
}
// Quick XIP check: can this .so be executed in place?  If not, skip the
// patch_main_imports step (which writes xip_cross_call into GOT slots meant
// for the nolib loader's dl_cross_call).
static bool can_xip_at_provision(std::span<const std::byte> image) noexcept {
    xip::SegmentMap seg;
    return xip::classify_segments(image, seg) && seg.split &&
           xip::text_is_immutable(image, seg);
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
        // Pre-resolve external calls into the .got.plt before flashing, so the
        // flash-resident PLT stubs (which read their GOT slot PC-relative) reach
        // main's functions once the R-X segment executes in place.
        // Only patch GOT for XIP-able libraries (split R-X|RW, immutable text).
        // Fused libraries are loaded copy-whole and their JUMP_SLOTs are set to
        // dl_cross_call by the nolib loader — xip_cross_call would fault because
        // the copy-whole handle is not in g_xip_handles.
        if (can_xip_at_provision(std::span<const std::byte>{buf, sz})) {
            int patched = xip::nolib::patch_main_imports(std::span<std::byte>{buf, sz});
            if (std::strcmp(name, "mylib1.so") == 0) g_patch1 = patched;
        }
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    if (!ok) trace_printf("  [prov] FAILED %s\n", name);
    return ok;
}



// ---- mylib1: full surface (aa=20, bb=40 after ctor; cc=5, dd=7) ----
void test_mylib1() {
    trace_printf("\n[test] ==== mylib1.so ====\n");
    void* h = xopen("mylib1.so");
    if (!h) { trace_printf("  dlopen FAILED\n"); return; }
    const std::uint32_t g = xip::nolib::get_got_base(h);
    void* lf = xip::nolib::dlsym(h, "library_function");
    trace_printf("  loaded: %s;  library_function @ 0x%08x (%s)\n",
                 xip::nolib::is_xip(h) ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", RD(h, "aa"));
    trace_printf("  bb                    = %u\n", RD(h, "bb"));
    // scalar functions (pure — before the bb-mutating library_function3)
    trace_printf("  library_function(39)  = %u\n", C1(h, g, "library_function", 39));
    trace_printf("  library_function2(10) = %u\n", C1(h, g, "library_function2", 10));
    trace_printf("  g(5)                  = %u\n", C1(h, g, "g", 5));
    trace_printf("  gg(5)                 = %u\n", C1(h, g, "gg", 5));
    trace_printf("  fff(5)                = %u\n", C1(h, g, "fff", 5));
    trace_printf("  callback_f(10)        = %u\n", C1(h, g, "callback_f", 10));
    trace_printf("  foo()                 = %u\n", C0(h, g, "foo"));
    trace_printf("  boo()                 = %u\n", C0(h, g, "boo"));
    trace_printf("  boo2()                = %u\n", C0(h, g, "boo2"));
    trace_printf("  lkj()                 = %u\n", C0(h, g, "lkj"));
    // strings via R_ARM_RELATIVE (literal in flash .rodata; pointer relocated)
    trace_printf("  retstr()              = \"%s\"\n", CSTR(h, g, "retstr"));
    trace_printf("  retstr_()             = \"%s\"\n", CSTR(h, g, "retstr_"));
    // struct global s_lib { char*a; float f; char*bb; uint b; uint c }
    if (void* p = xip::nolib::dlsym(h, "s_lib")) {
        auto* b = reinterpret_cast<std::uint8_t*>(p);
        trace_printf("  s_lib.a               = \"%s\"\n", *reinterpret_cast<char**>(b + 0));
        trace_printf("  s_lib.bb              = \"%s\"\n", *reinterpret_cast<char**>(b + 8));
        trace_printf("  s_lib.b / s_lib.c     = %u / %u\n",
                     *reinterpret_cast<std::uint32_t*>(b + 12), *reinterpret_cast<std::uint32_t*>(b + 16));
    }
    // void functions (side-effects on statics; just exercise the code path)
    C0(h, g, "print_lib");  C1(h, g, "print_lib2", 0);
    // mutating call LAST: library_function3(10) sets bb, uses main's w()
    trace_printf("  library_function3(10) = %u\n", C1(h, g, "library_function3", 10));
    trace_printf("  bb (after lib3)       = %u\n", RD(h, "bb"));

    xip::nolib::dlclose(h);
    trace_printf("  dlclose ok\n");
}

// ---- mylib2: full surface (aaa=20, bbb=40 after ctor; ccc=5, ddd=7) ----
void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so ====\n");
    void* h = xopen("mylib2.so");
    if (!h) { trace_printf("  dlopen FAILED\n"); return; }
    const std::uint32_t g = xip::nolib::get_got_base(h);
    void* lf = xip::nolib::dlsym(h, "lf");
    trace_printf("  loaded: %s;  lf @ 0x%08x (%s)\n",
                 xip::nolib::is_xip(h) ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");

    trace_printf("  aaa                   = %u\n", RD(h, "aaa"));
    trace_printf("  bbb                   = %u\n", RD(h, "bbb"));
    trace_printf("  xx                    = %u\n", RD(h, "xx"));
    trace_printf("  lf(39)                = %u\n", C1(h, g, "lf", 39));
    trace_printf("  lf2(10)               = %u\n", C1(h, g, "lf2", 10));
    trace_printf("  xxx()                 = %u\n", C0(h, g, "xxx"));   // uses main's printf + BB/AA
    trace_printf("  mm()                  = %u\n", C0(h, g, "mm"));
    if (void* p = xip::nolib::dlsym(h, "str2"))
        trace_printf("  str2                  = \"%s\"\n", *reinterpret_cast<char**>(p));
    trace_printf("  lf3(10)               = %u\n", C1(h, g, "lf3", 10));   // uses main's w()
    trace_printf("  bbb (after lf3)       = %u\n", RD(h, "bbb"));
    C0(h, g, "pl");                 // void (printf + bbb-=10)

    xip::nolib::dlclose(h);
    trace_printf("  dlclose ok\n");
}

} // namespace

// Declared in test_c1_c2_cpp3.cpp
void test_c1_c2_cpp3();
// Declared in test_ehx_cons_ppeh_ppsh_prov.cpp
void test_ehx_cons_ppeh_ppsh_prov();
// Declared in walkthrough.cpp: didactic flash-vs-RAM execution-address map.
void xip_walkthrough();
// Declared in walkthrough_complex.cpp: complex dynamic walkthrough.
void xip_walkthrough_complex();

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: keep lib printf in step with trace
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace_printf("\nscl-fw-xip-nolib1: full-surface XIP + cross-lib C/C++ test\n");

    // Enable cross-call routing so copy-whole libraries (c1→c2, cpp3↔c2) get the
    // correct r10 (got_base) when calling across library boundaries.
    fw::dl_enable_cross_call(true);

    trace_printf("[prov] formatting flat-fs\n");
    if (!fw::fs_format()) trace_printf("[prov] fs_format FAILED\n");
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

    test_mylib1();
    test_mylib2();
    test_c1_c2_cpp3();
    test_ehx_cons_ppeh_ppsh_prov();
    xip_walkthrough();
    xip_walkthrough_complex();

    trace_printf("\nscl-fw-xip-nolib1: done\n");
    for (;;) { }
}

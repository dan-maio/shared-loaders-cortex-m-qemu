/**
 * @file main.cpp
 * @brief Minimal no-pic static loader demo — call the fixtures DIRECTLY BY NAME.
 *
 * main declares the fixture exports as plain externs, links them (`-l:*.so`),
 * and calls them like ordinary functions: s_add(10, 5), cpp3_method(7, 5), ...
 * No r10, no GOT plumbing, no resolve()/casts in main. The loader's
 * xip::nopic::patch_main() fills main's own GOT/PLT slots at load.
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

// ---- The firmware surface the fixtures import -----------------------------
extern "C" {
void trace_event(const char* o, const char* f, std::uint32_t a) {
    trace_printf("[%s] %s(%u)\n", o, f, a); }
void trace_u32(const char* l, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", l, v); }
std::uint32_t fm_var = 123;
// NOTE: NOT const — a const namespace-scope global has internal linkage in C++
// and would never reach main's .dynsym for the fixtures to import.
struct FmInfo fm_info = {0x00F1, 0x0007};
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
unsigned int HOST_K = 1000;
unsigned int host_f(unsigned int x) { return x * 2 + 1; }
unsigned int AA = 42;
unsigned int BB = 84;
char*        s1 = const_cast<char*>("main-s1");
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
}
#include "link-anchor.hpp"

// ---- The fixtures' exports, declared by name (linked with -l:*.so) ---------
extern "C" {
// slib
extern unsigned int s_a;
extern unsigned int s_b;
extern const char*  s_msg;
struct SInfo { const char* tag; unsigned int x, y; };
extern struct SInfo s_info;
unsigned int s_add(unsigned int, unsigned int);
unsigned int s_scale(unsigned int);
unsigned int s_via_helper(unsigned int);
unsigned int s_apply(unsigned int);
const char*  s_text(void);
const char*  s_tag(void);
unsigned int s_get_a(void);
unsigned int s_get_b(void);
unsigned int s_use_host(unsigned int);
unsigned int s_mix(unsigned int);
void         s_print(void);
// libprov / libcons
unsigned int prov_value(unsigned int);
unsigned int cons_call(unsigned int);
// libcppsh (C++ class behind a C ABI)
typedef void* cppsh_widget;
cppsh_widget  cppsh_make(unsigned int);
unsigned int  cppsh_compute(cppsh_widget, unsigned int);
unsigned int  cppsh_instances(void);
void          cppsh_free(cppsh_widget);
// libcppeh / ehx_catch are NOT linked into main (see the note in the cppeh/ehx
// section) — they are called via xip::nopic::resolve() + a cast.
// cpp3 (C++ class + hard-float math)
extern const unsigned int cpp3_magic;
extern unsigned int       cpp3_var;
unsigned int cpp3_method(unsigned int, unsigned int);
unsigned int cpp3_heap(unsigned int);
unsigned int cpp3_orchestrate(unsigned int);
unsigned int cpp3_recurse(unsigned int);
unsigned int cpp3_float_cross(unsigned int);
unsigned int cpp3_calls(void);
unsigned int cpp3_made(void);
// c2
extern unsigned int       c2_data;
extern const unsigned int c2_const;
extern unsigned int       c2_bss;
extern const char*        c2_tag;
unsigned int c2_add(unsigned int, unsigned int);
unsigned int c2_param(void);
unsigned int c2_chain(unsigned int);
unsigned int c2_relay(unsigned int);
unsigned int c2_step_down(unsigned int);
float        c2_scale(float);
unsigned int c2_state(void);
// c1
extern unsigned int c1_data;
extern unsigned int c1_bss;
extern const char*  c1_tag;
unsigned int c1_use_fm(void);
unsigned int c1_call_c2(unsigned int);
unsigned int c1_into_fm(unsigned int);
unsigned int c1_state(void);
}

namespace {

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
    if (src.read_at(0, std::span<std::byte>{buf, sz}).has_value())
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    fw::image_arena().free(buf);
    if (!ok) trace_printf("  [prov] fs_add FAILED: %s\n", name);
    return ok;
}

}  // namespace

extern "C" int main(void) {
    trace_printf("\nscl-fw-xip-static-no-pic-simple: BY-NAME calls, no r10\n");

    trace_printf("[prov] formatting flat-fs\n");
    if (!fw::fs_format()) trace_printf("[prov] fs_format FAILED\n");
    for (const char* f : { "slib.so", "libprov.so", "libcons.so", "libcppsh.so",
                           "libcppeh.so", "ehx_catch.so", "cpp3.so", "c2.so", "c1.so" })
        provision(f);
    trace_printf("[boot] flat-fs files = %u\n", fw::fs_count());

    // ---- load everything; then fix module<->module and main<->module links ----
    scl::Module m_slib, m_prov, m_cons, m_cppsh, m_cppeh, m_ehx, m_cpp3, m_c2, m_c1;
    xload("slib.so", m_slib);
    xload("libprov.so", m_prov);
    xload("libcons.so", m_cons);
    xload("libcppsh.so", m_cppsh);
    xload("libcppeh.so", m_cppeh);
    xload("ehx_catch.so", m_ehx);
    xload("cpp3.so", m_cpp3);
    xload("c2.so", m_c2);
    xload("c1.so", m_c1);
    xip::nopic::resolve_all();      // cyclic module<->module imports
    xip::nopic::patch_main(m_slib.get()); // fill MAIN's GOT/PLT for by-name calls
    xip::nopic::patch_main(m_prov.get());
    xip::nopic::patch_main(m_cons.get());
    xip::nopic::patch_main(m_cppsh.get());
    xip::nopic::patch_main(m_cppeh.get());
    xip::nopic::patch_main(m_ehx.get());
    xip::nopic::patch_main(m_cpp3.get());
    xip::nopic::patch_main(m_c2.get());
    xip::nopic::patch_main(m_c1.get());

    // ================= slib =================
    trace_printf("\n-- slib --\n");
    trace_printf("  s_a=%u s_b=%u (ctor)  s_msg=\"%s\"  s_info.tag=\"%s\" x=%u y=%u\n",
                 s_a, s_b, s_msg, s_info.tag, s_info.x, s_info.y);
    trace_printf("  s_add(10,5)=%u  s_scale(3)=%u  s_via_helper(5)=%u  s_apply(3)=%u\n",
                 s_add(10, 5), s_scale(3), s_via_helper(5), s_apply(3));
    trace_printf("  s_text()=\"%s\"  s_tag()=\"%s\"  s_get_a()=%u s_get_b()=%u\n",
                 s_text(), s_tag(), s_get_a(), s_get_b());
    trace_printf("  s_use_host(5)=%u (host_f+HOST_K)  s_mix(4)=%u  s_get_b()=%u\n",
                 s_use_host(5), s_mix(4), s_get_b());
    trace_printf("  s_print(): "); s_print();

    // ================= libprov / libcons =================
    trace_printf("\n-- libprov / libcons --\n");
    trace_printf("  prov_value(5)=%u\n", prov_value(5));
    trace_printf("  cons_call(5)=%u (prov_value+1)\n", cons_call(5));

    // ================= libcppsh =================
    trace_printf("\n-- libcppsh --\n");
    trace_printf("  instances before=%u\n", cppsh_instances());
    cppsh_widget w = cppsh_make(7);
    trace_printf("  cppsh_make(7)=%p  instances after=%u\n", w, cppsh_instances());
    trace_printf("  cppsh_compute(w,5)=%u\n", cppsh_compute(w, 5));
    cppsh_free(w);
    trace_printf("  cppsh_free done  instances=%u\n", cppsh_instances());

    // ================= libcppeh / ehx_catch =================
    // These two are exception-enabled DSOs; linking them into main would pull
    // libgcc's HIDDEN __aeabi_unwind_cpp_pr1 through -Wl,-E ("hidden symbol
    // referenced by DSO"), so they are called via resolve() like every other
    // XIP app does. (Their throwing path needs module .ARM.exidx, CANTUNWIND.)
    trace_printf("\n-- libcppeh / ehx_catch --\n");
    auto eh_selftest = reinterpret_cast<int (*)(int)>(m_cppeh.sym("eh_selftest"));
    auto eh_dyncast  = reinterpret_cast<int (*)(int)>(m_cppeh.sym("eh_dyncast"));
    auto ehx_cross   = reinterpret_cast<unsigned (*)(unsigned)>(m_ehx.sym("ehx_catch_cross"));
    trace_printf("  eh_selftest(0)=%d (no throw)  eh_dyncast(0)=%d (RTTI downcast)\n",
                 eh_selftest(0), eh_dyncast(0));
    trace_printf("  ehx_catch_cross(0)=%u (eh_throw(0) normal return; throwing path\n",
                 ehx_cross(0));
    trace_printf("    skipped — module .ARM.exidx is not registered, CANTUNWIND)\n");

    // ================= cpp3 =================
    trace_printf("\n-- cpp3 --\n");
    trace_printf("  cpp3_magic=0x%08x  cpp3_var=0x%08x  made=%u  calls=%u\n",
                 cpp3_magic, cpp3_var, cpp3_made(), cpp3_calls());
    trace_printf("  cpp3_method(7,5)=%u  (Mathy::compute, sinf/cosf/logf/sqrtf from main)\n",
                 cpp3_method(7, 5));
    trace_printf("  cpp3_heap(8)=%u  (cross-module malloc/new -> main's heap)\n", cpp3_heap(8));
    trace_printf("  cpp3_orchestrate(5)=%u (cpp3->c2->cpp3->main)\n", cpp3_orchestrate(5));
    trace_printf("  cpp3_recurse(5)=%u (mutual recursion, sum 0..5)\n", cpp3_recurse(5));
    trace_printf("  cpp3_float_cross(10)=%u (float cpp3->c2, +c2_data)\n", cpp3_float_cross(10));
    trace_printf("  cpp3_calls=%u after\n", cpp3_calls());

    // ================= c2 =================
    trace_printf("\n-- c2 --\n");
    trace_printf("  c2_data=0x%08x  c2_const=0x%08x  c2_bss=0x%08x  c2_tag=\"%s\"  hits=%u\n",
                 c2_data, c2_const, c2_bss, c2_tag, c2_state());
    trace_printf("  c2_add(10,20)=%u  c2_param()=%u\n", c2_add(10, 20), c2_param());
    trace_printf("  c2_chain(3)=%u (c2->c1->main)\n", c2_chain(3));
    trace_printf("  c2_relay(4)=%u (c2->cpp3->main)\n", c2_relay(4));
    trace_printf("  c2_step_down(5)=%u (c2->cpp3 mutual recursion)\n", c2_step_down(5));
    trace_printf("  c2_scale(1.5f)=%d (float, +c2_data)\n", (int)c2_scale(1.5f));

    // ================= c1 =================
    trace_printf("\n-- c1 --\n");
    trace_printf("  c1_data=0x%08x  c1_bss=0x%08x  c1_tag=\"%s\"  state=%u\n",
                 c1_data, c1_bss, c1_tag, c1_state());
    trace_printf("  c1_use_fm()=%u (fm_var+fm_info+magic)\n", c1_use_fm());
    trace_printf("  c1_call_c2(7)=%u (c1->c2, +state+bss)\n", c1_call_c2(7));
    trace_printf("  c1_into_fm(2)=%u (c1->main callback)\n", c1_into_fm(2));
    trace_printf("  c1_state=%u after\n", c1_state());

    trace_printf("\nscl-fw-xip-static-no-pic-simple: done\n");
    std::exit(0);
    for (;;) { }
}

/**
 * @file main.cpp
 * @brief Plan 4b demo: dlopen/dlsym/call/dlclose mylib1 (C) and libcppsh (C++).
 *
 * Proves the on-target dynamic loader: load a host-built ARM .so over semihosting,
 * relocate it into the RAM arena, resolve its imports against main, run its
 * constructors, call into it (r10 set per-library via a trampoline), and unload.
 */
#include "board.h"
#include "semihost.h"
#include "dl.hpp"
#include "dl-semihost.hpp"
#include "lazy-plt.hpp"
#include "pic-trampoline.h"
#include "cross-error.hpp"   // 5d: shared exception type (same header the lib uses)
#include <cstdint>


// Semihosting opens paths relative to the OpenOCD CWD, which differs between a CLI
// run (repo root) and an Eclipse/VS Code launch (project dir). Use an absolute path
// to the committed fixtures, injected by the Makefile (-DDL_LIB_DIR=...).
#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

namespace {
/** @brief Print "label<decimal v>\n" via semihosting (no libc). */
void put_u32(const char* label, std::uint32_t v) {
    char b[12]; int i = 10; b[11] = '\n';
    if (v == 0) b[i--] = '0';
    while (v) { b[i--] = char('0' + v % 10); v /= 10; }
    trace_puts(label); trace_puts(b + i + 1);
}
} // namespace

/** @brief Demo entry: dlopen/dlsym/call/dlclose each fixture lib in turn, then spin. */
int main() {
    board_init();
    trace_puts("f411re-dynloader 4b: starting\n");

    // ---- mylib1 (C, single-pic-base) ----
#if 1
    if (void* h = dl::dlopen(DL_LIB_DIR "/mylib1.so", RTLD_NOW)) {
        auto* lib = reinterpret_cast<fw::handle*>(h);
        trace_puts("dlopen mylib1.so OK\n");
        if (void* fn = dl::dlsym(h, "library_function")) {
            std::uint32_t r = pic_call_u32(fn, reinterpret_cast<void*>(lib->got_base), 39);
            put_u32("library_function(39) = ", r);   // 39 + 3 + aa
        } else { trace_puts("dlsym library_function FAILED\n"); }
        dl::dlclose(h);
        trace_puts("dlclose mylib1 OK\n");
    } else { trace_puts("dlopen mylib1 FAILED\n"); }
#endif

    // ---- reload proof (arena slot reused) ----
    if (void* h = dl::dlopen(DL_LIB_DIR "/mylib1.so", RTLD_NOW)) { trace_puts("reload mylib1 OK\n"); dl::dlclose(h); }
    else { trace_puts("reload mylib1 FAILED\n"); }

    // ---- mylib2 (second C lib, single-pic-base) ----
    if (void* h = dl::dlopen(DL_LIB_DIR "/mylib2.so", RTLD_NOW)) {
        auto* lib = reinterpret_cast<fw::handle*>(h);
        trace_puts("dlopen mylib2.so OK\n");
        if (void* fn = dl::dlsym(h, "lf")) {
            std::uint32_t r = pic_call_u32(fn, reinterpret_cast<void*>(lib->got_base), 39);
            put_u32("lf(39) = ", r);   // 39 + 3 + aaa(10); ctor lives in .inits, not run
        } else { trace_puts("dlsym lf FAILED\n"); }
        dl::dlclose(h);
        trace_puts("dlclose mylib2 OK\n");
    } else { trace_puts("dlopen mylib2 FAILED\n"); }

    // ---- 5b: inter-library linking (libcons imports prov_value from libprov) ----
    if (void* hp = dl::dlopen(DL_LIB_DIR "/libprov.so", RTLD_NOW)) {
        auto* prov = reinterpret_cast<fw::handle*>(hp);
        trace_puts("dlopen libprov.so OK\n");
        if (void* hc = dl::dlopen(DL_LIB_DIR "/libcons.so", RTLD_NOW)) {
            auto* cons = reinterpret_cast<fw::handle*>(hc);
            trace_puts("dlopen libcons.so OK\n");
            if (void* fn = dl::dlsym(hc, "cons_call")) {
                std::uint32_t r = pic_call_u32(fn, reinterpret_cast<void*>(cons->got_base), 5);
                put_u32("cons_call(5) = ", r);          // prov_value(5)+1 = 5+100+1 = 106
            } else { trace_puts("dlsym cons_call FAILED\n"); }
            // Closing the provider first must be refused (consumer still imports it).
            dl::dlclose(hp);                           // expect "dlclose refused: ..." trace
            dl::dlclose(hc);                           // ok: drops prov's refcount to 0
            trace_puts("dlclose libcons OK\n");
            dl::dlclose(hp);                           // now ok
            trace_puts("dlclose libprov OK\n");
        } else { trace_puts("dlopen libcons FAILED\n"); dl::dlclose(hp); }
    } else { trace_puts("dlopen libprov FAILED\n"); }

    // ---- 5c: lazy PLT binding (cons_call's prov_value import binds on FIRST call) ----
    // The fixup counter is cumulative, so we assert on the *delta*: +1 on the first call
    // (resolver runs, GOT slot patched), +0 on the second (slot now points straight at prov).
    if (void* hp = dl::dlopen(DL_LIB_DIR "/libprov.so", RTLD_NOW)) {
        auto* prov = reinterpret_cast<fw::handle*>(hp);
        if (void* hc = dl::dlopen(DL_LIB_DIR "/libcons.so", RTLD_NOW)) {
            auto* cons = reinterpret_cast<fw::handle*>(hc);
            trace_puts("5c: dlopen libprov+libcons OK\n");
            if (void* fn = dl::dlsym(hc, "cons_call")) {
                void* got = reinterpret_cast<void*>(cons->got_base);
                std::uint32_t c0 = fw::dl_fixup_count();
                std::uint32_t r1 = pic_call_u32(fn, got, 5);   // first call -> lazy fixup
                std::uint32_t c1 = fw::dl_fixup_count();
                std::uint32_t r2 = pic_call_u32(fn, got, 5);   // second call -> patched slot
                std::uint32_t c2 = fw::dl_fixup_count();
                put_u32("5c cons_call#1 = ", r1);               // 106
                put_u32("5c cons_call#2 = ", r2);               // 106
                put_u32("5c fixups before = ", c0);
                put_u32("5c fixups after #1 = ", c1);            // c0 + 1
                put_u32("5c fixups after #2 = ", c2);            // == c1
            } else { trace_puts("5c dlsym cons_call FAILED\n"); }
            dl::dlclose(hc);
            dl::dlclose(hp);
            trace_puts("5c dlclose OK\n");
        } else { trace_puts("5c dlopen libcons FAILED\n"); dl::dlclose(hp); }
    } else { trace_puts("5c dlopen libprov FAILED\n"); }

    // ---- libcppsh (C++23, -fPIC, operator new resolved from main) ----
    if (void* h = dl::dlopen(DL_LIB_DIR "/libcppsh.so", RTLD_NOW)) {
        auto* lib = reinterpret_cast<fw::handle*>(h);
        void* got = reinterpret_cast<void*>(lib->got_base);
        trace_puts("dlopen libcppsh.so OK\n");
        void* mk = dl::dlsym(h, "cppsh_make");
        void* cp = dl::dlsym(h, "cppsh_compute");
        void* ic = dl::dlsym(h, "cppsh_instances");
        void* fr = dl::dlsym(h, "cppsh_free");
        if (mk && cp && ic && fr) {
            std::uint32_t w  = pic_call_u32(mk, got, 7);          // cppsh_make(7)
            std::uint32_t n1 = pic_call_u32(ic, got, 0);          // cppsh_instances()
            std::uint32_t r  = pic_call_u32_2(cp, got, w, 5); // cppsh_compute(w, 5)
            put_u32("cppsh_instances (after make) = ", n1);
            put_u32("cppsh_compute(w, 5) = ", r);
            pic_call_u32(fr, got, w);   // cppsh_free(w)
            std::uint32_t n2 = pic_call_u32(ic, got, 0);
            put_u32("cppsh_instances (after free) = ", n2);
        } else { trace_puts("dlsym cppsh_* FAILED\n"); }
        dl::dlclose(h);
        trace_puts("dlclose libcppsh OK\n");
    } else { trace_puts("dlopen libcppsh FAILED\n"); }

    // ---- 5d: C++ exceptions + RTTI across the boundary (libcppeh) ----
    // The EH/RTTI runtime (__cxa_*, __gxx_personality_v0, __dynamic_cast, __cxxabiv1
    // vtables) resolves from main; the loader registered the lib's relocated .ARM.exidx
    // so a throw inside the lib can be unwound.
    if (void* h = dl::dlopen(DL_LIB_DIR "/libcppeh.so", RTLD_NOW)) {
        auto* lib = reinterpret_cast<fw::handle*>(h);
        void* got = reinterpret_cast<void*>(lib->got_base);
        trace_puts("dlopen libcppeh.so OK\n");
        void* self = dl::dlsym(h, "eh_selftest");
        void* thr  = dl::dlsym(h, "eh_throw");
        void* dyn  = dl::dlsym(h, "eh_dyncast");
        if (self && thr && dyn) {
            // (a) intra-lib throw+catch: unwinds the lib's own frames.
            put_u32("5d eh_selftest = ", pic_call_u32(self, got, 1));        // 98
            // (b) cross-boundary: the lib throws, the exception propagates through the
            //     pic_call trampoline frame, and main catches it by typeinfo NAME.
            std::uint32_t code = 0xFFFFFFFFu;
            try {
                pic_call_u32(thr, got, 1);                                  // throw cross_error(7)
            } catch (const cross_error& e) {
                code = e.code();
            }
            put_u32("5d cross-boundary catch code = ", code);                // 7
            // (c) dynamic_cast on lib-defined polymorphic types.
            put_u32("5d dynamic_cast who = ", pic_call_u32(dyn, got, 0));    // 2
        } else { trace_puts("5d dlsym eh_* FAILED\n"); }
        dl::dlclose(h);
        trace_puts("5d dlclose OK\n");
    } else { trace_puts("dlopen libcppeh FAILED\n"); }

    trace_puts("f411re-dynloader 4b: done\n");
    semihost_exit(0);
    for (;;) { led_toggle(); delay_ms(500); }
}

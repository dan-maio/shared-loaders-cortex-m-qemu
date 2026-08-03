/**
 * @file main-tests.cpp
 * @brief Plan 6 cross-module test battery driver (NOT the stock main.cpp).
 *
 * Selected only by `make -f Makefile_TESTS` (which swaps this in for main.cpp), so the
 * normal firmware/demo is untouched. Drives 15 tests over c1 (C), c2 (C), cpp3 (C++) --
 * plus libcppeh + ehx_catch for the exception test -- with cross-library r10 routing so
 * lib->lib calls run with the callee's own got_base. Tests 1-7 cover the cross-module
 * call graphs (the GOT/PLT walkthrough is verbose around test 7); tests 8-16 cover
 * lifecycle (dlclose refcount, reload), binding modes (lazy vs deferred), the error
 * contract, and the ABI edges (exception unwinding and hard-float, both THROUGH the
 * cross-call trampoline). Each library traces its own calls with provenance.
 *
 * BUILD (firmware, see Makefile FW_* + Makefile_TESTS overrides):
 *   compile: arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
 *            -fpic -std=c++23 -O0 -g -ffreestanding -Ifirmware/src -Icore/include
 *            -Ilibs/include -Ilibs/libcppeh/include -DDL_LIB_DIR=<abs fixtures path>
 *   link:    -pie -nostdlib -nostartfiles -Wl,-Bsymbolic -Wl,-E --gc-sections
 *            <objs> libsupc++.a libgcc.a libm.a, with -u anchors keeping the FM-exported
 *            trace/data symbols and the float math entries (sinf/cosf/logf/sqrtf) alive.
 *   start:   reset vector -> startup.c -> main(); the only "start parameter" is
 *            -DDL_LIB_DIR so semihosting dlopen is CWD-independent.
 */
#include "board.h"
#include "semihost.h"
#include "dl.hpp"
#include "dl-semihost.hpp"
#include "dl-fs.hpp"
#include "lazy-plt.hpp"   // fw::dl_fixup_count() (test 11: lazy vs deferred binding)
#include "pic-trampoline.h"
#include "fm-iface.h"
#include "loader/error.hpp"
#include "battery.hpp"      // flat_fs: g_use_flatfs hook + battery_run() entry
#include <cstdint>
#include <new>     // operator new/delete for the heap test (test 13)

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

// Test 13 (heap): malloc/free are defined in main's crt.cpp, backed by the same
// region_allocator as operator new. Declared here so the battery can exercise them.
extern "C" void* malloc(unsigned int n) noexcept;
extern "C" void  free(void* p) noexcept;

// ===========================================================================
// FM-exported surface (resolved by loaded libs through the loader's link_context).
// Defined here, kept in main's .dynsym by -u anchors, exported via -Wl,-E.
// ===========================================================================
namespace {
/** @brief Print @p v as decimal over semihosting (no libc). */
void emit_dec(std::uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { trace_putc('0'); return; }
    while (v) { b[i++] = char('0' + v % 10); v /= 10; }
    while (i) trace_putc(b[--i]);
}
} // namespace

extern "C" {
/** @brief FM trace: emit one event with provenance: "[origin] fn(arg)\n". */
void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_putc('['); trace_puts(origin); trace_puts("] "); trace_puts(fn);
    trace_putc('('); emit_dec(arg); trace_puts(")\n");
}
/** @brief FM trace: emit "label<decimal v>\n". */
void trace_u32(const char* label, std::uint32_t v) {
    trace_puts(label); emit_dec(v); trace_putc('\n');
}
std::uint32_t          fm_var  = 170u;            ///< FM-owned mutable global a lib consumes (test 5). 0x00AA
const struct FmInfo    fm_info = { 241u, 7u };    ///< FM-owned const aggregate (test 5). {id=0x00F1, flags=0x07}
/** @brief FM function a library calls back into (tests 2 and 6); returns x + 256. */
std::uint32_t fm_callback(std::uint32_t x) {
    trace_event("FM", "fm_callback", x);
    return x + 0x100u;                            // +256
}
} // extern "C"

// ===========================================================================
// flat_fs hook (see battery.hpp). Default false -> the stock battery loads from
// host semihosting; main-flatfs.cpp sets it true to load from the on-chip FS.
// ===========================================================================
bool g_use_flatfs = false;

// ===========================================================================
// Test harness.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;   ///< Running pass / fail tallies for the battery.

/**
 * @brief Open a library from host semihosting (by path) or, in the flat_fs build, from
 *        the on-chip flat archive (by basename). Single seam used everywhere the battery
 *        opens a library, so the 15 tests are identical across both firmwares.
 */
loader::result<fw::handle*> open_sel(const char* path) {
    if (!g_use_flatfs) return fw::dlopen(path);
    const char* b = path;
    for (const char* p = path; *p; ++p) if (*p == '/') b = p + 1;   // basename
    return fw::dlopen_fs(b);
}

/** @brief Print "label0xXXXXXXXX" (8 hex digits), so each test narrates itself with live addresses. */
void put_hex(const char* label, std::uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char out[9]; for (int i = 7; i >= 0; --i) { out[i] = hx[v & 0xfu]; v >>= 4; }
    out[8] = '\0'; trace_puts(label); trace_puts(out);
}
/** @brief Print a "  - step" narration line. */
void step(const char* msg) { trace_puts("  - "); trace_puts(msg); trace_putc('\n'); }

/** @brief Assert @p got == @p want; print "TESTn PASS/FAIL" and bump the tally. */
void check(int n, std::uint32_t got, std::uint32_t want) {
    trace_puts("TEST"); emit_dec(static_cast<std::uint32_t>(n));
    if (got == want) { trace_puts(" PASS got="); emit_dec(got); trace_putc('\n'); ++g_pass; }
    else { trace_puts(" FAIL got="); emit_dec(got); trace_puts(" want="); emit_dec(want);
           trace_putc('\n'); ++g_fail; }
}
/** @brief Assert @p cond; print "TESTn PASS/FAIL val=" and bump the tally. */
void check_ok(int n, bool cond, std::uint32_t shown) {
    trace_puts("TEST"); emit_dec(static_cast<std::uint32_t>(n));
    if (cond) { trace_puts(" PASS val="); emit_dec(shown); trace_putc('\n'); ++g_pass; }
    else { trace_puts(" FAIL val="); emit_dec(shown); trace_putc('\n'); ++g_fail; }
}

/** @brief dlopen @p path, trace base/got_base, return the handle (nullptr on failure). */
fw::handle* open_lib(const char* path) {
    auto h = open_sel(path);
    if (!h) { trace_puts("  - dlopen FAILED: "); trace_puts(path); trace_putc('\n'); return nullptr; }
    trace_puts("  - dlopen "); trace_puts(path);
    put_hex("  -> base=0x", (*h)->base); put_hex(" got_base=0x", (*h)->got_base);
    trace_puts("  (ctors ran during this call)\n");
    return *h;
}
/** @brief dlsym @p n in @p h, trace its address, return it (nullptr on failure). */
void* sym(fw::handle& h, const char* n) {
    auto f = fw::dlsym(h, n);
    if (!f) { trace_puts("  - dlsym FAILED: "); trace_puts(n); trace_putc('\n'); return nullptr; }
    trace_puts("  - dlsym "); trace_puts(n);
    put_hex("  -> 0x", static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(*f)));
    trace_putc('\n');
    return *f;
}
/**
 * @brief dlclose every handle in @p hs whose refcount reached 0, repeatedly until all
 *        are unloaded (handles any dependency order, including mutual lib<->lib imports).
 */
void close_all(fw::handle** hs, int n) {
    step("dlclose all (runs destructors, frees image, decrements refcounts)");
    for (int pass = 0; pass < n; ++pass)
        for (int i = 0; i < n; ++i)
            if (hs[i] && hs[i]->in_use && hs[i]->refcount == 0) fw::dlclose(*hs[i]);
}
/** @brief The got_base of @p h as a void* (the r10 value for entering it). */
inline void* gotp(fw::handle& h) { return reinterpret_cast<void*>(h.got_base); }
} // namespace

/**
 * @brief The 15-test battery (enable cross-call routing, run the tests, then spin).
 *
 * Named battery_run() so the flat_fs driver (main-flatfs.cpp) can provision the
 * filesystem and then call it. A weak main() at the bottom of this file makes the
 * stock semihosting battery self-contained.
 */
int battery_run() {
    board_init();
    trace_puts("\nf411re-dynloader Plan 6: cross-module battery\n");
    fw::dl_enable_cross_call(true);   // route lib->lib calls through dl_cross_call (r10 swap)
    fw::dl_cross_trace(1);            // trace cross-lib GOT/PLT hops

    // -------- TEST 1: FM -> c1 -> c2 (lib->lib, callee touches own .data/.bss) --------
    {
        trace_puts("\n== TEST 1: FM -> c1 -> c2 ==\n");
        fw::handle* c2 = open_lib(DL_LIB_DIR "/c2.so");
        fw::handle* c1 = open_lib(DL_LIB_DIR "/c1.so");
        if (c1 && c2) {
            if (void* fn = sym(*c1, "c1_call_c2")) {
                step("call FM -> c1.c:c1_call_c2(5)  (c1 hits own .data/.bss, cross-calls c2_add)");
                std::uint32_t r = pic_call_u32(fn, gotp(*c1), 5);
                check(1, r, 17467u);   // see plan: 13096 + count(2) + c1_bss(4369)
            }
        }
        fw::handle* hs[] = { c1, c2 }; close_all(hs, 2);
    }

    // -------- TEST 2: FM -> c2 -> c1 -> (FM)  (two-hop, last hop into main) --------
    {
        trace_puts("\n== TEST 2: FM -> c2 -> c1 -> (FM) ==\n");
        fw::handle* c1 = open_lib(DL_LIB_DIR "/c1.so");
        fw::handle* c2 = open_lib(DL_LIB_DIR "/c2.so");
        if (c1 && c2) {
            if (void* fn = sym(*c2, "c2_chain")) {
                step("call FM -> c2.c:c2_chain(5)  (c2 -> c1 -> back into FM)");
                std::uint32_t r = pic_call_u32(fn, gotp(*c2), 5);
                check(2, r, 13704u);   // 13510 + c2.tag(194)
            }
        }
        fw::handle* hs[] = { c1, c2 }; close_all(hs, 2);
    }

    // -------- TEST 3: FM ->(CM) cpp3 (hard-float sin/cos/log/sqrt via main) --------
    {
        trace_puts("\n== TEST 3: FM ->(CM) cpp3 (hard-float math) ==\n");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (cpp3) {
            if (void* fn = sym(*cpp3, "cpp3_method")) {
                step("call FM ->(CM) cpp3.cpp:Mathy::compute via cpp3_method(seed=2, x=100)");
                std::uint32_t r = pic_call_u32_2(fn, gotp(*cpp3), 2, 100);
                check_ok(3, r > 0u && r < 1000000u, r);   // hard-float result is finite
            }
        }
        fw::handle* hs[] = { cpp3 }; close_all(hs, 1);
    }

    // -------- TEST 4: FM uses const + var from c2 (read exported data) --------
    {
        trace_puts("\n== TEST 4: FM uses var + const from c2 ==\n");
        fw::handle* c2 = open_lib(DL_LIB_DIR "/c2.so");
        if (c2) {
            step("FM reads c2's exported .data (c2_data) + .rodata (c2_const) via dlsym");
            auto* dv = static_cast<volatile std::uint32_t*>(sym(*c2, "c2_data"));
            auto* cv = static_cast<volatile std::uint32_t*>(sym(*c2, "c2_const"));
            std::uint32_t d = dv ? *dv : 0u, c = cv ? *cv : 0u;
            trace_u32("  c2_data  = ", d);
            trace_u32("  c2_const = ", c);
            check(4, (d == 0x2200u && c == 0x02C0u) ? 1u : 0u, 1u);
        }
        fw::handle* hs[] = { c2 }; close_all(hs, 1);
    }

    // -------- TEST 5: c1 uses var + struct ("class") from FM --------
    {
        trace_puts("\n== TEST 5: c1 uses var + struct from FM ==\n");
        fw::handle* c1 = open_lib(DL_LIB_DIR "/c1.so");
        if (c1) {
            if (void* fn = sym(*c1, "c1_use_fm")) {
                step("call FM -> c1.c:c1_use_fm()  (c1 reads FM's fm_var + fm_info aggregate)");
                std::uint32_t r = pic_call_u32(fn, gotp(*c1), 0);
                check(5, r, 611u);   // fm_var(170)+id(241)+flags(7)+c1.magic(193)
            }
        }
        fw::handle* hs[] = { c1 }; close_all(hs, 1);
    }

    // -------- TEST 6: FM uses class/var/const from cpp3; cpp3 -> c2 -> cpp3 -> (FM) --------
    {
        trace_puts("\n== TEST 6: FM uses cpp3 class/var/const; cpp3 -> c2 -> cpp3 -> (FM) ==\n");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        fw::handle* c2   = open_lib(DL_LIB_DIR "/c2.so");
        if (cpp3 && c2) {
            auto* mg = static_cast<volatile std::uint32_t*>(sym(*cpp3, "cpp3_magic"));
            auto* vr = static_cast<volatile std::uint32_t*>(sym(*cpp3, "cpp3_var"));
            trace_u32("  cpp3_magic = ", mg ? *mg : 0u);
            trace_u32("  cpp3_var   = ", vr ? *vr : 0u);
            if (void* mk = sym(*cpp3, "cpp3_made"))  // observe the class/ctor state
                trace_u32("  cpp3_made  = ", pic_call_u32(mk, gotp(*cpp3), 0));
            if (void* fn = sym(*cpp3, "cpp3_orchestrate")) {
                step("call FM -> cpp3.cpp:cpp3_orchestrate(1)  (cpp3 -> c2 -> cpp3 -> FM, 3 hops)");
                std::uint32_t r = pic_call_u32(fn, gotp(*cpp3), 1);
                check(6, r, 34441u);   // 21334 + cpp3_magic(13107)
            }
        }
        fw::handle* hs[] = { cpp3, c2 }; close_all(hs, 2);
    }

    // -------- TEST 7: FM ->(CM) cpp3 with params FROM c2 + full GOT/PLT translation trace --
    {
        trace_puts("\n== TEST 7: FM ->(CM) cpp3 with params from c2 (verbose GOT/PLT) ==\n");
        fw::handle* c2   = open_lib(DL_LIB_DIR "/c2.so");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (c2 && cpp3) {
            std::uint32_t x = 0, seed = 0;
            if (void* pf = sym(*c2, "c2_param")) {
                step("call FM -> c2.c:c2_param()  (parameter source for cpp3_method)");
                x = pic_call_u32(pf, gotp(*c2), 0);          // 0x2200 + 0x02C0 = 9408
            }
            auto* cv = static_cast<volatile std::uint32_t*>(sym(*c2, "c2_const"));
            seed = cv ? *cv : 0u;                            // 704
            trace_u32("  param x (from c2_param) = ", x);
            trace_u32("  param seed (from c2_const) = ", seed);

            // ---- STEP 1 + STEP 3: the didactic GOT walkthrough for cpp3 -- the .got as
            //      loaded from the file (BEFORE) versus after the loader relocated it and
            //      routed every function slot to the cross-call/lazy trampoline (AFTER). ----
            fw::dl_dump_got_walkthrough(*cpp3);

            // ---- STEP 2 (the other side): where cpp3's lib->main slots actually resolve. ----
            trace_puts("\n[STEP 2] Resolving cpp3's external (lib->main) imports:\n");
            static const char* prov[] = { "trace_event","trace_u32","sinf","cosf","logf","sqrtf" };
            fw::dl_dump_main_provides(prov, 6);

            // ---- STEP 4: dlsym() walks the same machinery to hand back public addresses. ----
            trace_puts("\n[STEP 4] dlsym() - symbol resolution through the patched image:\n");
            static const char* exps[] = { "cpp3_magic","cpp3_var","cpp3_made",
                                          "cpp3_method","cpp3_orchestrate" };
            for (const char* nm : exps) (void) sym(*cpp3, nm);

            if (void* fn = sym(*cpp3, "cpp3_method")) {
                // ---- live translation: dl_cross_trace(2) makes the loader print
                //      slot=.. tgt=.. got=.. for every hop as the trampoline resolves it. ----
                trace_puts("\n  --- live GOT/PLT translation during the call (cross_trace=2) ---\n");
                fw::dl_cross_trace(2);   // verbose: trace every GOT/PLT translation stage
                step("call cpp3_method(seed, x) ->(CM) Mathy::compute (hard-float via main)");
                std::uint32_t r = pic_call_u32_2(fn, gotp(*cpp3), seed, x);
                fw::dl_cross_trace(1);

                // ---- prove the cross-call design DEFERS -- slots are re-resolved each
                //      call, so the GOT still points at the trampoline (not patched). ----
                trace_puts("\n  --- post-call snapshot (function slots unchanged: deferred) ---\n");
                fw::dl_dump_image(*cpp3);

                // ---- STEP 5: verify each relocation TYPE did its job, by observed value. ----
                trace_puts("\n[STEP 5] Verifying each relocation type:\n");
                auto* mg = static_cast<volatile std::uint32_t*>(sym(*cpp3, "cpp3_magic"));
                auto* vr = static_cast<volatile std::uint32_t*>(sym(*cpp3, "cpp3_var"));
                trace_u32("    RELATIVE : cpp3_magic (rebased local) = ", mg ? *mg : 0u);
                trace_u32("    GLOB_DAT : cpp3_var   (data symbol)   = ", vr ? *vr : 0u);
                trace_u32("    JUMP_SLOT: cpp3_method(seed,x) via cross-call -> main math = ", r);
                trace_puts("    (finite, non-zero result => every reloc type resolved)\n");

                check_ok(7, r > 0u && r < 1000000u, r);
            }
            trace_puts("\nGOT/PLT WALKTHROUGH COMPLETE\n");
        }
        fw::handle* hs[] = { c2, cpp3 }; close_all(hs, 2);
    }

    // -------- TEST 8: dlclose refcount refusal across a real cross-lib dependency ----
    {
        trace_puts("\n== TEST 8: dlclose refused while a cross-lib dependent is loaded ==\n");
        fw::handle* c2   = open_lib(DL_LIB_DIR "/c2.so");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");   // imports c2_relay -> depends on c2
        if (c2 && cpp3) {
            step("dlclose(c2) while cpp3 still imports from it -> must be REFUSED");
            fw::dlclose(*c2);                          // refcount != 0 -> refused, handle untouched
            bool refused = c2->in_use;                 // still loaded => refusal worked
            step("dlclose(cpp3) -> drops c2's refcount to 0");
            fw::dlclose(*cpp3);
            step("dlclose(c2) again -> now frees");
            fw::dlclose(*c2);
            bool freed = !c2->in_use;
            check_ok(8, refused && freed, refused ? (freed ? 2u : 1u) : 0u);  // want 2
        }
    }

    // -------- TEST 9: .bss/ctor state persists across calls, RESETS on reload ----------
    {
        trace_puts("\n== TEST 9: state persists across calls, resets on reload ==\n");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (cpp3) {
            void* mth = sym(*cpp3, "cpp3_method");
            void* cnt = sym(*cpp3, "cpp3_calls");
            if (mth && cnt) {
                step("call cpp3_method 3x (each Mathy ctor bumps the .bss call counter)");
                for (int i = 0; i < 3; ++i) (void) pic_call_u32_2(mth, gotp(*cpp3), 2, 100);
                std::uint32_t after3 = pic_call_u32(cnt, gotp(*cpp3), 0);   // expect 3
                trace_u32("  cpp3_calls after 3 calls = ", after3);
                fw::handle* h0[] = { cpp3 }; close_all(h0, 1);

                step("dlclose + reopen -> ctor re-runs, counter must RESET");
                fw::handle* cpp3b = open_lib(DL_LIB_DIR "/cpp3.so");
                std::uint32_t reset = 999;
                if (cpp3b) {
                    void* mth2 = sym(*cpp3b, "cpp3_method");
                    void* cnt2 = sym(*cpp3b, "cpp3_calls");
                    if (mth2 && cnt2) {
                        (void) pic_call_u32_2(mth2, gotp(*cpp3b), 2, 100);  // 1 call
                        reset = pic_call_u32(cnt2, gotp(*cpp3b), 0);        // expect 1, not 4
                        trace_u32("  cpp3_calls after reload + 1 call = ", reset);
                    }
                    fw::handle* h1[] = { cpp3b }; close_all(h1, 1);
                }
                check_ok(9, after3 == 3u && reset == 1u, after3 * 10u + reset);  // want 31
            }
        }
    }

    // -------- TEST 10: C++ exception thrown in one lib, caught in another, UNWOUND --
    //         THROUGH the dl_cross_call trampoline (libcppeh throws -> ehx catches). ----
    {
        trace_puts("\n== TEST 10: exception unwinds lib -> dl_cross_call -> lib catch ==\n");
        fw::handle* eh  = open_lib(DL_LIB_DIR "/libcppeh.so");   // defines eh_throw (the thrower)
        fw::handle* ehx = open_lib(DL_LIB_DIR "/ehx_catch.so");  // imports eh_throw cross-lib
        if (eh && ehx) {
            if (void* fn = sym(*ehx, "ehx_catch_cross")) {
                step("call ehx_catch_cross(1) -> cross-calls eh_throw -> throw cross_error(7)");
                std::uint32_t r = pic_call_u32(fn, gotp(*ehx), 1);   // 1000 + 7 if caught
                check(10, r, 1007u);   // proves the throw unwound back through the trampoline
            }
        }
        fw::handle* hs[] = { ehx, eh }; close_all(hs, 2);
    }

    // -------- TEST 11: lazy binding (dl_runtime_resolve) vs. deferred cross-call --------
    {
        trace_puts("\n== TEST 11: lazy PLT fixup fires once, then never again ==\n");
        fw::dl_enable_cross_call(false);   // external JUMP_SLOTs -> dl_runtime_resolve (lazy)
        fw::handle* c2 = open_lib(DL_LIB_DIR "/c2.so");
        fw::handle* c1 = open_lib(DL_LIB_DIR "/c1.so");   // c1's fm_callback/trace_event = lazy
        if (c1 && c2) {
            if (void* fn = sym(*c1, "c1_into_fm")) {
                std::uint32_t c0 = fw::dl_fixup_count();
                step("first call c1_into_fm -> lazy-binds its main imports (fixup_count rises)");
                (void) pic_call_u32(fn, gotp(*c1), 5);
                std::uint32_t c1f = fw::dl_fixup_count();
                step("second call -> slots already patched, NO new fixups");
                (void) pic_call_u32(fn, gotp(*c1), 5);
                std::uint32_t c2f = fw::dl_fixup_count();
                trace_u32("  fixups before = ", c0);
                trace_u32("  fixups after 1st call = ", c1f);
                trace_u32("  fixups after 2nd call = ", c2f);
                check_ok(11, (c1f > c0) && (c2f == c1f), (c1f > c0) ? (c2f == c1f ? 2u : 1u) : 0u);
            }
        }
        fw::handle* hs[] = { c1, c2 }; close_all(hs, 2);
        fw::dl_enable_cross_call(true);    // restore deferred cross-call for the rest
    }

    // -------- TEST 12: error paths -- FileNotFound, NotElf, SymbolNotFound ------------
    {
        trace_puts("\n== TEST 12: loader error contract (negative paths) ==\n");
        // loader::Error codes: Ok=0 FileNotFound=1 ShortRead=2 NotElf=3 NotArm32=4
        //                      NotSharedObject=5 BadSection=6 SymbolNotFound=7 ...
        auto ecode = [](auto& r) -> std::uint32_t {
            return r.has_value() ? 0u : static_cast<std::uint32_t>(r.err());
        };
        step("dlopen a missing path -> FileNotFound");
        auto miss = open_sel(DL_LIB_DIR "/does_not_exist.so");
        std::uint32_t c_miss = ecode(miss);
        bool e_miss = !miss.has_value() && miss.err() == loader::error::FILE_NOT_FOUND;
        step("dlopen a non-ELF blob -> NotElf");
        auto junk = open_sel(DL_LIB_DIR "/notelf.bin");
        std::uint32_t c_junk = ecode(junk);
        bool e_junk = !junk.has_value() && junk.err() == loader::error::NOT_ELF;
        step("dlsym a missing symbol in a good lib -> SymbolNotFound");
        bool e_sym = false; std::uint32_t c_sym = 99;
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (cpp3) {
            auto bad = fw::dlsym(*cpp3, "no_such_symbol_here");
            c_sym = ecode(bad);
            e_sym = !bad.has_value() && bad.err() == loader::error::SYMBOL_NOT_FOUND;
            fw::handle* hs[] = { cpp3 }; close_all(hs, 1);
        }
        trace_u32("  missing-file  error code = ", c_miss);   // expect 1 (FileNotFound)
        trace_u32("  non-ELF blob  error code = ", c_junk);   // expect 3 (NotElf)
        trace_u32("  missing-sym   error code = ", c_sym);    // expect 7 (SymbolNotFound)
        check_ok(12, e_miss && e_junk && e_sym, (e_miss?4u:0u)+(e_junk?2u:0u)+(e_sym?1u:0u)); // 7
    }

    // -------- TEST 13: heap -- new/delete, malloc/free, coalesce/reuse, alignment, ----
    //          plus cross-module: a loaded lib (cpp3) allocates from MAIN's heap. -------
    {
        trace_puts("\n== TEST 13: heap (new/delete, malloc/free, reuse) + cross-module ==\n");
        auto aligned = [](void* p) { return (reinterpret_cast<std::uintptr_t>(p) & 7u) == 0u; };

        // (a) operator new / delete (scalar): value round-trips, 8-byte aligned.
        auto* one = new std::uint32_t(0x0000ABCDu);
        bool ok_new = one && aligned(one) && (*one == 0x0000ABCDu);
        put_hex("  - new u32        @ 0x",
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(one)));
        trace_putc('\n');
        delete one;

        // (b) operator new[] / delete[] (array): fill 1..8, sum == 36.
        const std::uint32_t N = 8u;
        auto* arr = new std::uint32_t[N];
        std::uint32_t asum = 0u;
        if (arr) { for (std::uint32_t i = 0; i < N; ++i) arr[i] = i + 1u;
                   for (std::uint32_t i = 0; i < N; ++i) asum += arr[i]; }
        bool ok_arr = arr && aligned(arr) && (asum == 36u);
        delete[] arr;

        // (c) malloc / free: write a byte pattern, read it back.
        auto* buf = static_cast<unsigned char*>(malloc(64));
        bool ok_malloc = buf && aligned(buf);
        if (buf) {
            for (int i = 0; i < 64; ++i) buf[i] = static_cast<unsigned char>(i ^ 0x5A);
            for (int i = 0; i < 64; ++i)
                if (buf[i] != static_cast<unsigned char>(i ^ 0x5A)) ok_malloc = false;
            free(buf);
        }

        // (d) coalesce + reuse: two adjacent 64-byte blocks, freed, must be reclaimed as
        //     one 128-byte span at the SAME base (proves free() returns memory + merges).
        auto* a = static_cast<unsigned char*>(malloc(64));
        auto* b = static_cast<unsigned char*>(malloc(64));
        bool adjacent = a && b && (b == a + 64);            // front-split first-fit -> contiguous
        free(a); free(b);                                    // free both -> allocator coalesces
        auto* c = static_cast<unsigned char*>(malloc(128));  // fits only via the merged span
        bool ok_reuse = adjacent && (c == a);                // reclaimed the SAME base
        put_hex("  - blkA @ 0x",
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a)));
        put_hex("  reuse(128) @ 0x",
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(c)));
        trace_putc('\n');
        free(c);

        // (e) cross-module: cpp3 mallocs/news from MAIN's heap (its malloc/new are UND,
        //     resolved against main's crt.cpp through the loader). cpp3_heap(8) -> 533.
        std::uint32_t xr = 0u;
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (cpp3) {
            if (void* fn = sym(*cpp3, "cpp3_heap")) {
                step("call cpp3_heap(8): lib malloc/new resolve to MAIN's heap -> 533");
                xr = pic_call_u32(fn, gotp(*cpp3), 8);
            }
            fw::handle* hs[] = { cpp3 }; close_all(hs, 1);
        }
        bool ok_xmod = (xr == 533u);

        trace_u32("  array sum (1..8) = ", asum);
        trace_u32("  cpp3_heap(8)     = ", xr);
        check_ok(13, ok_new && ok_arr && ok_malloc && ok_reuse && ok_xmod,
                 (ok_new?16u:0u)+(ok_arr?8u:0u)+(ok_malloc?4u:0u)+(ok_reuse?2u:0u)+(ok_xmod?1u:0u)); // want 31
    }

    // -------- TEST 14: deep re-entrant cross-lib recursion (r10 integrity at depth) ----
    {
        trace_puts("\n== TEST 14: deep mutual cross-recursion cpp3 <-> c2 (depth 6) ==\n");
        fw::handle* c2   = open_lib(DL_LIB_DIR "/c2.so");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (c2 && cpp3) {
            if (void* fn = sym(*cpp3, "cpp3_recurse")) {
                step("call cpp3_recurse(6): cpp3->c2->cpp3->...->0, summing 6+5+..+1");
                std::uint32_t r = pic_call_u32(fn, gotp(*cpp3), 6);
                check(14, r, 21u);   // 6+5+4+3+2+1+0; each level touched its own .bss via own r10
            }
        }
        fw::handle* hs[] = { c2, cpp3 }; close_all(hs, 2);
    }

    // -------- TEST 15: hard-float passed CROSS-LIB through dl_cross_call (VFP s0) -------
    {
        trace_puts("\n== TEST 15: float crosses cpp3 -> c2 through the trampoline ==\n");
        fw::handle* c2   = open_lib(DL_LIB_DIR "/c2.so");
        fw::handle* cpp3 = open_lib(DL_LIB_DIR "/cpp3.so");
        if (c2 && cpp3) {
            if (void* fn = sym(*cpp3, "cpp3_float_cross")) {
                step("call cpp3_float_cross(10): float 10.0 -> c2_scale (s0) -> 10 + c2_data");
                std::uint32_t r = pic_call_u32(fn, gotp(*cpp3), 10);
                check(15, r, 8714u);   // 10 + 0x2200(8704): proves d0-d7 + r10 preserved
            }
        }
        fw::handle* hs[] = { c2, cpp3 }; close_all(hs, 2);
    }

    // -------- TEST 16: two simultaneous instances of the same lib (state isolation) ----
    {
        trace_puts("\n== TEST 16: two cpp3 instances keep independent .bss/r10 ==\n");
        fw::handle* A = open_lib(DL_LIB_DIR "/cpp3.so");
        fw::handle* B = open_lib(DL_LIB_DIR "/cpp3.so");   // second image, distinct base/got
        if (A && B) {
            void* mA = sym(*A, "cpp3_method");
            void* cA = sym(*A, "cpp3_calls");
            void* cB = sym(*B, "cpp3_calls");
            if (mA && cA && cB) {
                step("call instance A's cpp3_method once; B must be untouched");
                (void) pic_call_u32_2(mA, gotp(*A), 2, 100);     // A.calls -> 1
                std::uint32_t a = pic_call_u32(cA, gotp(*A), 0); // expect 1
                std::uint32_t b = pic_call_u32(cB, gotp(*B), 0); // expect 0
                trace_u32("  A.cpp3_calls = ", a);
                trace_u32("  B.cpp3_calls = ", b);
                check_ok(16, a == 1u && b == 0u, a * 10u + b);   // want 10
            }
        }
        fw::handle* hs[] = { A, B }; close_all(hs, 2);
    }

    // -------- summary --------
    trace_puts("\n== BATTERY SUMMARY: ");
    emit_dec(static_cast<std::uint32_t>(g_pass)); trace_puts("/");
    emit_dec(static_cast<std::uint32_t>(g_pass + g_fail));
    trace_puts(g_fail == 0 ? " PASS ==\n" : " (FAILURES) ==\n");
    trace_puts("battery: done\n");

    for (;;) { led_toggle(); delay_ms(500); }
}

// Stock entry point for the semihosting battery (Makefile_TESTS). WEAK so the
// flat_fs firmware's own main() (main-flatfs.cpp), which provisions the on-chip
// filesystem first and then calls battery_run(), overrides it at link time.
__attribute__((weak)) int main() { return battery_run(); }

/**
 * @file main-overlay.cpp
 * @brief OVERLAY firmware: runs three scenarios TWICE -- once via the dynamic loader
 *        (dlopen of c1/c2/cpp3 over semihosting, runtime relocation), once via static
 *        linker overlays (one shared SRAM window, link-time address binding) -- and
 *        prints the resolved addresses side by side so the two address-resolution
 *        machines can be compared directly.
 *
 * THE TWO MECHANISMS
 *  - Dynamic loading: the loader (dl.cpp) allocates a RAM image per library at a
 *    runtime base, applies relocations (R_ARM_RELATIVE/GLOB_DAT/JUMP_SLOT), patches
 *    the GOT/PLT, and runs constructors. Each .so gets its OWN base; symbols resolve
 *    to per-load addresses and several libraries can be co-resident.
 *  - Overlays: the three modules are linked INTO this image, stored back-to-back in
 *    FLASH, and copied on demand into ONE shared SRAM window. The linker bound every
 *    address at LINK TIME; "loading" is a memcpy + DSB/ISB. All three modules ALIAS
 *    the same window VMA, so only one can be valid at a time (NOCROSSREFS guarantees
 *    they never reference each other).
 *
 * SAFETY (per the STM32F411 overlay rules): the overlay runs from SRAM; the only ISR
 * we enable (SysTick, in board.c/FLASH) never touches the window, so a context switch
 * cannot corrupt an in-flight overlay. The F411 has no instruction cache, so a DSB+ISB
 * after the memcpy (flush the pipeline) is sufficient -- no cache maintenance needed.
 */
#include "board.h"
#include "semihost.h"
#include "dl.hpp"
#include "dl-semihost.hpp"
#include "overlay.h"
#include "pic-trampoline.h"
#include "fm-iface.h"
#include "loader/error.hpp"
#include <cstdint>
#include <cstring>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

// ===========================================================================
// FM-exported surface (resolved by the dynamically-loaded libs through the loader's
// link_context, AND referenced directly by the static overlay modules). Defined here,
// kept in main's .dynsym by -u anchors (Makefile_TESTS), exported via -Wl,-E.
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
std::uint32_t       fm_var  = 170u;           ///< FM-owned mutable global a module reads (0x00AA).
const struct FmInfo fm_info = { 241u, 7u };   ///< FM-owned const aggregate {id=0x00F1, flags=0x07}.
/** @brief FM function a module calls back into; returns x + 256. */
std::uint32_t fm_callback(std::uint32_t x) {
    trace_event("FM", "fm_callback", x);
    return x + 0x100u;
}
} // extern "C"

// ===========================================================================
// Overlay symbols from sections-overlay.ld + overlay-resident entry points.
// The CALLS into the window cross ~400MB (FLASH main -> SRAM window), beyond Thumb
// BL reach, so the entries are long_call (register-indirect BLX). Exported DATA is
// read through main's -fpic GOT (a slot holding the window address -- range-free).
// ===========================================================================
extern "C" {
extern std::uint8_t _ovl_vma[];          ///< Shared window VMA (== _ovl_window, 0x20018000).
extern std::uint8_t _ovl_window_size[];  ///< Window size in bytes (symbol value, not storage).
extern std::uint8_t _ov1_lma[],   _ov1_size[];    ///< ov1 module: FLASH load address + byte size.
extern std::uint8_t _ov2_lma[],   _ov2_size[];    ///< ov2 module: FLASH load address + byte size.
extern std::uint8_t _ovcpp_lma[], _ovcpp_size[];  ///< ovcpp module: FLASH load address + byte size.

/** @brief ov1 entry: reads fm_var/fm_info + calls main; returns 611 (== c1_use_fm). */
__attribute__((long_call)) std::uint32_t ov1_use_fm(void);
/** @brief ov1: current value of its in-window call counter. */
__attribute__((long_call)) std::uint32_t ov1_count(void);
/** @brief ov2 entry: returns ov2_data + ov2_const (calls main to trace). */
__attribute__((long_call)) std::uint32_t ov2_sum(void);
/** @brief ovcpp entry: C++ class method, returns seed*x + ovcpp_magic. */
__attribute__((long_call)) std::uint32_t ovcpp_method(std::uint32_t seed, std::uint32_t x);
/** @brief ovcpp hard-float entry: same FPU formula as cpp3_method (sinf/cosf/logf/sqrtf). */
__attribute__((long_call)) std::uint32_t ovcpp_fcompute(std::uint32_t seed, std::uint32_t x);
/** @brief ovcpp heap entry: malloc/free + new/delete from MAIN's heap; (8)->533 (== cpp3_heap). */
__attribute__((long_call)) std::uint32_t ovcpp_heap(std::uint32_t n);

extern std::uint32_t       ov2_data;     ///< ov2-exported .data (0x2200), in the window.
extern const std::uint32_t ov2_const;    ///< ov2-exported .rodata (0x02C0), in the window.
extern const std::uint32_t ovcpp_magic;  ///< ovcpp-exported const (0x3333), in the window.
extern std::uint32_t       ovcpp_var;    ///< ovcpp-exported .data (0x3030), in the window.
} // extern "C"

namespace {

/** @brief A pointer as a 32-bit address value (for printing/comparison). */
inline std::uint32_t u32(const void* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
}
/** @brief Print "label0xXXXXXXXX" (8 hex digits). */
void put_hex(const char* label, std::uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char out[9]; for (int i = 7; i >= 0; --i) { out[i] = hx[v & 0xfu]; v >>= 4; }
    out[8] = '\0'; trace_puts(label); trace_puts(out);
}
int g_pass = 0, g_fail = 0;   ///< Running pass / fail tallies for the comparison battery.
/** @brief Assert @p cond; print "TESTn PASS/FAIL got=" and bump the tally. */
void check(int n, bool cond, std::uint32_t shown) {
    trace_puts("TEST"); emit_dec(static_cast<std::uint32_t>(n));
    if (cond) { trace_puts(" PASS got="); emit_dec(shown); trace_putc('\n'); ++g_pass; }
    else      { trace_puts(" FAIL got="); emit_dec(shown); trace_putc('\n'); ++g_fail; }
}

// ---- the overlay manager: the SCL descriptor-driven window manager ----
/** @brief Which overlay module is currently resident (indices into the table). */
enum class Ov : int { None = SCL_OVERLAY_NONE, Ov1 = 0, Ov2 = 1, OvCpp = 2 };

scl_overlay_module g_ovmods[3];  ///< Descriptor table (filled from linker symbols in ov_init).
scl_overlay_region g_ovl;        ///< The shared window + residency state.

/** @brief Build the descriptor table from the sections-overlay.ld symbols. */
void ov_init() {
    g_ovmods[0] = { _ovl_vma, _ov1_lma,   u32(_ov1_size),   "ov1"   };
    g_ovmods[1] = { _ovl_vma, _ov2_lma,   u32(_ov2_size),   "ov2"   };
    g_ovmods[2] = { _ovl_vma, _ovcpp_lma, u32(_ovcpp_size), "ovcpp" };
    scl_overlay_init(&g_ovl, g_ovmods, 3);
}
/** @brief Make @p which resident (memcpy FLASH->window + DSB/ISB; no-op if resident). */
void ov_load(Ov which) { (void)scl_overlay_load(&g_ovl, static_cast<int>(which)); }

// ---- dynamic-loader helpers (the comparison's "dynamic" half) ----
/** @brief dlopen @p path, trace its runtime base/got_base, return the handle (nullptr on failure). */
fw::handle* dyn_open(const char* path) {
    auto h = fw::dlopen(path);
    if (!h) { trace_puts("  - dlopen FAILED: "); trace_puts(path); trace_putc('\n'); return nullptr; }
    trace_puts("  - dlopen "); trace_puts(path);
    put_hex("  base=0x", (*h)->base); put_hex(" got_base=0x", (*h)->got_base); trace_putc('\n');
    return *h;
}
/** @brief dlsym @p n in @p h, trace its runtime address, return it (nullptr on failure). */
void* dyn_sym(fw::handle& h, const char* n) {
    auto f = fw::dlsym(h, n);
    if (!f) { trace_puts("  - dlsym FAILED: "); trace_puts(n); trace_putc('\n'); return nullptr; }
    trace_puts("  - dlsym "); trace_puts(n); put_hex("  -> 0x", u32(*f)); trace_putc('\n');
    return *f;
}
/** @brief The got_base of @p h as a void* (r10 value for the pic_call trampolines). */
inline void* gotp(fw::handle& h) { return reinterpret_cast<void*>(h.got_base); }

} // namespace

// ===========================================================================
// The comparison battery.
// ===========================================================================
/** @brief Entry: run the four overlay-vs-dynamic comparison tests, print the verdict, spin. */
int main() {
    board_init();
    ov_init();
    trace_puts("\nf411re-dynloader: OVERLAY vs DYNAMIC-LOADING comparison\n");
    put_hex("shared overlay window @ 0x", u32(_ovl_vma));
    trace_puts(" size="); emit_dec(u32(_ovl_window_size)); trace_puts(" bytes\n");

    // ===== TEST 1 (mirrors battery TEST 5): module reads main's var/const/struct =====
    //       and calls back into main. Both mechanisms must return 611.
    {
        trace_puts("\n== TEST 1: module READS fm_var/fm_info + CALLS main (== c1_use_fm, want 611) ==\n");
        std::uint32_t dyn = 0, ovl = 0;
        trace_puts(" [dynamic] dlopen c1.so, dlsym c1_use_fm, call:\n");
        if (fw::handle* c1 = dyn_open(DL_LIB_DIR "/c1.so")) {
            if (void* fn = dyn_sym(*c1, "c1_use_fm")) dyn = pic_call_u32(fn, gotp(*c1), 0);
            fw::dlclose(*c1);
        }
        trace_puts(" [overlay] load ov1 into the window, call ov1_use_fm:\n");
        ov_load(Ov::Ov1);
        put_hex("  - &ov1_use_fm = 0x", u32(reinterpret_cast<void*>(&ov1_use_fm))); trace_putc('\n');
        ovl = ov1_use_fm();
        trace_u32("  dynamic c1_use_fm = ", dyn);
        trace_u32("  overlay ov1_use_fm = ", ovl);
        check(1, dyn == 611u && ovl == 611u, ovl);
    }

    // ===== TEST 2 (mirrors battery TEST 4): main READS module-exported .data + .rodata =====
    {
        trace_puts("\n== TEST 2: main READS module .data + .rodata (== c2_data 0x2200 / c2_const 0x02C0) ==\n");
        std::uint32_t dd = 0, dc = 0, od = 0, oc = 0;
        trace_puts(" [dynamic] dlopen c2.so, dlsym c2_data/c2_const, read:\n");
        if (fw::handle* c2 = dyn_open(DL_LIB_DIR "/c2.so")) {
            if (auto* p = static_cast<volatile std::uint32_t*>(dyn_sym(*c2, "c2_data")))  dd = *p;
            if (auto* p = static_cast<volatile std::uint32_t*>(dyn_sym(*c2, "c2_const"))) dc = *p;
            fw::dlclose(*c2);
        }
        trace_puts(" [overlay] load ov2 into the window, read ov2_data/ov2_const:\n");
        ov_load(Ov::Ov2);
        put_hex("  - &ov2_data = 0x",  u32(&ov2_data));
        put_hex("   &ov2_const = 0x",  u32(&ov2_const)); trace_putc('\n');
        od = ov2_data; oc = ov2_const;
        trace_u32("  dynamic c2_data = ", dd);  trace_u32("  dynamic c2_const = ", dc);
        trace_u32("  overlay ov2_data = ", od); trace_u32("  overlay ov2_const = ", oc);
        check(2, dd == 0x2200u && dc == 0x02C0u && od == 0x2200u && oc == 0x02C0u, od + oc);
    }

    // ===== TEST 3 (mirrors battery TEST 3/6, C++): class method + exported const =====
    {
        trace_puts("\n== TEST 3: C++ class method + exported const (== cpp3_magic 0x3333 / 13107) ==\n");
        std::uint32_t dmag = 0, omag = 0, omethod = 0;
        trace_puts(" [dynamic] dlopen cpp3.so, dlsym cpp3_magic/cpp3_method:\n");
        if (fw::handle* cpp3 = dyn_open(DL_LIB_DIR "/cpp3.so")) {
            if (auto* p = static_cast<volatile std::uint32_t*>(dyn_sym(*cpp3, "cpp3_magic"))) dmag = *p;
            (void) dyn_sym(*cpp3, "cpp3_method");   // show its runtime address (don't call: needs libm)
            fw::dlclose(*cpp3);
        }
        trace_puts(" [overlay] load ovcpp into the window, read ovcpp_magic, call ovcpp_method:\n");
        ov_load(Ov::OvCpp);
        put_hex("  - &ovcpp_magic = 0x", u32(&ovcpp_magic));
        put_hex("  &ovcpp_method = 0x",  u32(reinterpret_cast<void*>(&ovcpp_method))); trace_putc('\n');
        omag = ovcpp_magic;
        omethod = ovcpp_method(2u, 100u);           // Calc(2).compute(100) = 2*100 + 0x3333 = 13307
        trace_u32("  dynamic cpp3_magic = ", dmag);
        trace_u32("  overlay ovcpp_magic = ", omag);
        trace_u32("  overlay ovcpp_method(2,100) = ", omethod);
        check(3, dmag == 0x3333u && omag == 0x3333u && omethod == 13307u, omag);
    }

    // ===== TEST 4: the headline contrast -- address aliasing vs independent bases =====
    {
        trace_puts("\n== TEST 4: overlays ALIAS one window; dynamic libs get INDEPENDENT bases ==\n");
        const std::uint32_t win = u32(_ovl_vma), wsz = u32(_ovl_window_size);
        const std::uint32_t a1 = u32(reinterpret_cast<void*>(&ov1_use_fm))  & ~1u;
        const std::uint32_t a2 = u32(reinterpret_cast<void*>(&ov2_sum))     & ~1u;
        const std::uint32_t a3 = u32(reinterpret_cast<void*>(&ovcpp_method))& ~1u;
        trace_puts(" [overlay] all three entry points fall inside the SAME window:\n");
        put_hex("   ov1_use_fm  = 0x", a1); put_hex("   ov2_sum = 0x", a2);
        put_hex("   ovcpp_method = 0x", a3); trace_putc('\n');
        const bool aliased = (a1 >= win && a1 < win + wsz) &&
                             (a2 >= win && a2 < win + wsz) &&
                             (a3 >= win && a3 < win + wsz);

        trace_puts(" [dynamic] c1.so + c2.so + cpp3.so co-resident, three DISTINCT bases:\n");
        fw::handle* c1   = dyn_open(DL_LIB_DIR "/c1.so");
        fw::handle* c2   = dyn_open(DL_LIB_DIR "/c2.so");
        fw::handle* cpp3 = dyn_open(DL_LIB_DIR "/cpp3.so");
        bool distinct = false;
        if (c1 && c2 && cpp3) {
            distinct = c1->base != c2->base && c2->base != cpp3->base && c1->base != cpp3->base;
            put_hex("   c1.base = 0x", c1->base); put_hex("  c2.base = 0x", c2->base);
            put_hex("  cpp3.base = 0x", cpp3->base); trace_putc('\n');
        }
        if (cpp3) fw::dlclose(*cpp3);
        if (c2)   fw::dlclose(*c2);
        if (c1)   fw::dlclose(*c1);
        trace_puts(aliased  ? "  => overlays SHARE one VMA (time-multiplexed)\n"
                            : "  => UNEXPECTED: overlay entries not in window\n");
        trace_puts(distinct ? "  => dynamic libs hold INDEPENDENT VMAs (space-multiplexed)\n"
                            : "  => UNEXPECTED: dynamic bases not distinct\n");
        check(4, aliased && distinct, (aliased ? 2u : 0u) + (distinct ? 1u : 0u));   // want 3
    }

    // ===== TEST 5: hard-float computation, dynamic vs overlay (same FPU + libm) =====
    //       Both run the identical single-precision formula (sinf/cosf/logf/sqrtf via
    //       main's libm); the result must match (within float rounding) across the two
    //       address machines -- runtime-relocated JUMP_SLOTs vs link-time long_call BLX.
    {
        trace_puts("\n== TEST 5: hard-float compute dynamic vs overlay (FPU + libm; must match) ==\n");
        std::uint32_t dyn = 0, ovl = 0;
        trace_puts(" [dynamic] dlopen cpp3.so, call cpp3_method(2,100) (libm via relocated PLT):\n");
        if (fw::handle* cpp3 = dyn_open(DL_LIB_DIR "/cpp3.so")) {
            if (void* fn = dyn_sym(*cpp3, "cpp3_method"))
                dyn = pic_call_u32_2(fn, gotp(*cpp3), 2u, 100u);
            fw::dlclose(*cpp3);
        }
        trace_puts(" [overlay] load ovcpp, call ovcpp_fcompute(2,100) (same libm via long_call):\n");
        ov_load(Ov::OvCpp);
        put_hex("  - &ovcpp_fcompute = 0x", u32(reinterpret_cast<void*>(&ovcpp_fcompute)));
        trace_putc('\n');
        ovl = ovcpp_fcompute(2u, 100u);
        trace_u32("  dynamic cpp3_method(2,100)    = ", dyn);
        trace_u32("  overlay ovcpp_fcompute(2,100) = ", ovl);
        const std::uint32_t diff = dyn > ovl ? dyn - ovl : ovl - dyn;   // tolerate FP rounding
        check(5, dyn > 0u && dyn < 1000000u && diff <= 2u, ovl);
    }

    // ===== TEST 6: heap usage, dynamic vs overlay (both share MAIN's one heap) =====
    //       cpp3.so (dynamic) and the ovcpp overlay (static) each do malloc/free AND
    //       new/delete. All four operators are defined ONCE, in main (crt.cpp's
    //       region_allocator): the dynamic lib reaches them through relocated JUMP_SLOTs,
    //       the overlay through link-time-bound long_call BLX. Same heap, two address
    //       machines -- so the checksums must match (both == 533).
    {
        trace_puts("\n== TEST 6: heap dynamic vs overlay (malloc/free + new/delete; one heap) ==\n");
        std::uint32_t dyn = 0, ovl = 0;
        trace_puts(" [dynamic] dlopen cpp3.so, call cpp3_heap(8) (heap ops via relocated PLT):\n");
        if (fw::handle* cpp3 = dyn_open(DL_LIB_DIR "/cpp3.so")) {
            if (void* fn = dyn_sym(*cpp3, "cpp3_heap"))
                dyn = pic_call_u32(fn, gotp(*cpp3), 8u);
            fw::dlclose(*cpp3);
        }
        trace_puts(" [overlay] load ovcpp, call ovcpp_heap(8) (same heap via long_call BLX):\n");
        ov_load(Ov::OvCpp);
        put_hex("  - &ovcpp_heap = 0x", u32(reinterpret_cast<void*>(&ovcpp_heap)));
        trace_putc('\n');
        ovl = ovcpp_heap(8u);
        trace_u32("  dynamic cpp3_heap(8)  = ", dyn);
        trace_u32("  overlay ovcpp_heap(8) = ", ovl);
        check(6, dyn == 533u && ovl == 533u, ovl);
    }

    // ---- summary ----
    trace_puts("\n== OVERLAY-COMPARISON SUMMARY: ");
    emit_dec(static_cast<std::uint32_t>(g_pass)); trace_puts("/");
    emit_dec(static_cast<std::uint32_t>(g_pass + g_fail));
    trace_puts(g_fail == 0 ? " PASS ==\n" : " (FAILURES) ==\n");
    trace_puts("overlay: done\n");

    semihost_exit(0);
    for (;;) { led_toggle(); delay_ms(500); }
}

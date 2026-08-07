/**
 * @file main.cpp
 * @brief scl::reloc battery — load ET_REL modules (built -mlong-calls -fno-pic
 *        -r), resolve their imports against main and ACROSS the set, run their
 *        constructors, call exported functions, and verify.
 *
 * No GOT, no PLT, no r10, no lazy binding: every cross-module reference is an
 * R_ARM_ABS32 the loader patches at load time, so a resolved export is a plain
 * function pointer. Because ET_REL binds eagerly, the mutually-dependent
 * fixtures (c1<->c2<->cpp3) must be linked as a SET (reloc::load_set), which
 * places all modules first, then resolves — the eager analogue of the ET_DYN
 * loaders' lazy dl_cross_call.
 */
#include "board.h"
#include "semihost.h"
#include "reloc/reloc-loader.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// ---- main's exported ABI (what the modules import) --------------------------
extern "C" {
void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_putc('['); trace_puts(origin); trace_puts("] "); trace_puts(fn); trace_putc('(');
    char b[10]; int i = 0; std::uint32_t v = arg;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_puts(")\n");
}
void trace_u32(const char* label, std::uint32_t v) {
    trace_puts(label);
    char b[10]; int i = 0;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_putc('\n');
}
std::uint32_t          fm_var  = 170u;
const struct FmInfo { std::uint32_t id, flags; } fm_info = { 0xF1u, 7u };
std::uint32_t fm_callback(std::uint32_t x) { trace_event("FM", "fm_callback", x); return x + 0x100u; }

// libm / libc the C++ fixture (cpp3) imports; provided by main (libm + crt).
float sinf(float); float cosf(float); float logf(float); float sqrtf(float);
void* malloc(std::size_t); void free(void*);
// newlib's errno accessor, referenced by libm's logf/sqrtf; we are -nostdlib.
int* __errno(void) { static int e; return &e; }

// mylib1 & mylib2 exports
std::uint32_t AA = 5;
std::uint32_t BB = 55;
const char* s1 = "Help....\n";
std::uint32_t f(std::uint32_t x) { return 3 * x; }
std::uint32_t w(std::uint32_t x) { return AA + x; }

float __aeabi_d2f(double);
double __aeabi_f2d(float);

void trace_u32_raw(std::uint32_t v) {
    char b[10]; int i = 0;
    if (!v) trace_putc('0');
    else {
        while (v) { b[i++] = char('0' + v % 10); v /= 10; }
        while (i) trace_putc(b[--i]);
    }
}

int printf(const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    trace_puts("[printf] ");
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 'u') {
                unsigned int val = __builtin_va_arg(args, unsigned int);
                trace_u32_raw(val);
                format++;
            } else if (*format == 's') {
                const char* s = __builtin_va_arg(args, const char*);
                trace_puts(s);
                format++;
            } else {
                trace_putc('%');
                trace_putc(*format++);
            }
        } else {
            trace_putc(*format++);
        }
    }
    __builtin_va_end(args);
    return 0;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

double log(double x) {
    return static_cast<double>(logf(static_cast<float>(x)));
}
}

namespace {

template <class F> std::uint32_t addr(F f) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(f));
}

// Resolve main's exports by computing the address AT CALL TIME. (A static table
// of {name, addr(&fn)} would need dynamic init via main's own .init_array, which
// our bare Reset_Handler does not run — it would stay all-zero. Inline is safe.)
loader::result<std::uint32_t> main_resolve(void*, std::string_view n) {
    if (n == "trace_event") return addr(&trace_event);
    if (n == "trace_u32")   return addr(&trace_u32);
    if (n == "fm_callback") return addr(&fm_callback);
    if (n == "fm_var")      return addr(&fm_var);
    if (n == "fm_info")     return addr(&fm_info);
    if (n == "sinf")        return addr(&sinf);
    if (n == "cosf")        return addr(&cosf);
    if (n == "logf")        return addr(&logf);
    if (n == "sqrtf")       return addr(&sqrtf);
    if (n == "malloc")      return addr(&malloc);
    if (n == "free")        return addr(&free);
    if (n == "_Znwj")       return addr(static_cast<void* (*)(std::size_t)>(::operator new));
    if (n == "_ZdlPvj")     return addr(static_cast<void (*)(void*, std::size_t)>(::operator delete));
    if (n == "AA")          return addr(&AA);
    if (n == "BB")          return addr(&BB);
    if (n == "s1")          return addr(&s1);
    if (n == "f")           return addr(&f);
    if (n == "w")           return addr(&w);
    if (n == "printf")      return addr(&printf);
    if (n == "strcat")      return addr(&strcat);
    if (n == "log")         return addr(&log);
    if (n == "__aeabi_d2f") return addr(&__aeabi_d2f);
    if (n == "__aeabi_f2d") return addr(&__aeabi_f2d);
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

// Read a whole file over semihosting into @p buf; returns byte count (0 on error).
std::size_t read_file(const char* path, std::byte* buf, std::size_t cap) {
    int h = semihost_open(path, 1);
    if (h < 0) return 0;
    long n = semihost_flen(h);
    std::size_t sz = 0;
    if (n > 0 && static_cast<std::size_t>(n) <= cap &&
        semihost_read(h, buf, static_cast<int>(n)) == 0)
        sz = static_cast<std::size_t>(n);
    semihost_close(h);
    return sz;
}

// resident storage, reused across tests: up to 3 module images + one arena.
alignas(8) std::byte g_img[3][8 * 1024];
alignas(8) std::byte g_arena[8 * 1024];
std::span<const std::byte> g_spans[3];
reloc::module_set g_set;

// Load a set of ET_REL modules (by RELOC_LIB_DIR-relative basename) as one link
// unit; main resolves anything the set does not define.
bool load_set_of(const char* const* names, int n) {
    if (n > 3) return false;
    for (int i = 0; i < n; ++i) {
        // Bounded: RELOC_LIB_DIR is a build-time define, and a deep enough
        // target directory made these three unchecked writes run past the end
        // of path[] and smash the stack -- the firmware died on its first
        // module open, looking like a loader fault. Same guard style the
        // flatfs and spi-flash tests already use.
        char path[256]; std::size_t k = 0;
        for (const char* p = RELOC_LIB_DIR "/"; *p && k + 1 < sizeof path; ++p) path[k++] = *p;
        for (const char* p = names[i]; *p && k + 1 < sizeof path; ++p) path[k++] = *p;
        for (const char* p = ".rel.o"; *p && k + 1 < sizeof path; ++p) path[k++] = *p;
        path[k] = 0;
        std::size_t sz = read_file(path, g_img[i], sizeof g_img[i]);
        if (!sz) { trace_puts("  read FAILED: "); trace_puts(path); trace_putc('\n'); return false; }
        g_spans[i] = std::span<const std::byte>(g_img[i], sz);
    }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(g_spans, n), std::span(g_arena), rs);
    if (!r) {
        trace_puts("  load_set FAILED with error: ");
        trace_u32("", static_cast<std::uint32_t>(r.err()));
        return false;
    }
    g_set = *r;
    return true;
}

using u32_fn0 = std::uint32_t (*)();
using u32_fn1 = std::uint32_t (*)(std::uint32_t);
using u32_fn2 = std::uint32_t (*)(std::uint32_t, std::uint32_t);
template <class F> F fn(std::string_view s) { return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(g_set, s))); }

int g_pass = 0, g_fail = 0;
void check(const char* label, bool ok) {
    trace_puts("  ["); trace_puts(ok ? "PASS" : "FAIL"); trace_puts("] "); trace_puts(label); trace_putc('\n');
    if (ok) ++g_pass; else ++g_fail;
}

}  // namespace

int main() {
    board_init();
    trace_puts("\n==== scl::reloc — ET_REL module BATTERY ====\n");
    trace_puts("(-mlong-calls -fno-pic -r: no GOT, no PLT, no r10, no lazy binding)\n");

    // ---- TEST 1: relprov + relmod (acyclic: relmod imports prov_add) --------
    trace_puts("\n== TEST 1: relprov + relmod (cross-module ABS32) ==\n");
    { const char* names[] = { "relprov", "relmod" };
      if (load_set_of(names, 2)) {
        auto rel_compute   = fn<u32_fn1>("rel_compute");
        auto rel_read_data = fn<u32_fn0>("rel_read_data");
        auto prov_state    = fn<u32_fn0>("prov_state");
        std::uint32_t r = rel_compute ? rel_compute(100) : 0;
        trace_u32("  rel_compute(100) = ", r);
        check("rel_compute == 14307", r == 14307u);
        check("rel_read_data == 0x1100", rel_read_data && rel_read_data() == 0x1100u);
        check("prov_state == 1", prov_state && prov_state() == 1u);
      } else check("load relprov+relmod", false);
    }

    // ---- TEST 2: c1 + c2 + cpp3 (CYCLIC: must be one load_set) --------------
    trace_puts("\n== TEST 2: c1 + c2 + cpp3 (cyclic set, load_set) ==\n");
    { const char* names[] = { "c1", "c2", "cpp3" };
      if (load_set_of(names, 3)) {
        auto c1_call_c2 = fn<u32_fn1>("c1_call_c2");
        auto c1_state   = fn<u32_fn0>("c1_state");
        auto c1_use_fm  = fn<u32_fn0>("c1_use_fm");
        auto cpp3_recurse = fn<u32_fn1>("cpp3_recurse");
        auto cpp3_method  = fn<u32_fn2>("cpp3_method");

        // c1 -> c2_add (cross-module). c1 is STATEFUL (bumps its own .bss counter),
        // so the 2nd call is the 1st + 1. Exact values match the ET_DYN loaders'
        // dyn2 battery — proof that eager ET_REL linking is behaviourally identical.
        std::uint32_t a = c1_call_c2 ? c1_call_c2(5) : 0;
        std::uint32_t b = c1_call_c2 ? c1_call_c2(5) : 0;
        trace_u32("  c1_call_c2(5) = ", a);
        check("c1_call_c2(5) == 17467 (== ET_DYN)", a == 17467u);
        check("c1_call_c2 stateful: 2nd call differs (.bss persists)", b > a);
        check("c1_state tracked the calls", c1_state && c1_state() > 0u);

        // c1 reads main's fm_var/fm_info (absolute data references)
        std::uint32_t u = c1_use_fm ? c1_use_fm() : 0;
        trace_u32("  c1_use_fm() = ", u);
        check("c1_use_fm() == 611 (reads FM data)", u == 611u);

        // cpp3 <-> c2 mutual cross-recursion (r10-free, absolute calls)
        std::uint32_t d = cpp3_recurse ? cpp3_recurse(6) : 0;
        trace_u32("  cpp3_recurse(6) = ", d);
        check("cpp3_recurse(6) == 21 (6+5+..+1)", d == 21u);

        // cpp3 hard-float math via main's libm (sinf/cosf/logf/sqrtf)
        std::uint32_t m1 = cpp3_method ? cpp3_method(2, 100) : 0;
        std::uint32_t m2 = cpp3_method ? cpp3_method(2, 100) : 0;
        trace_u32("  cpp3_method(2,100) = ", m1);
        check("cpp3_method(2,100) == 1432 (libm)", m1 == 1432u && m1 == m2);
      } else check("load_set c1+c2+cpp3", false);
    }

    // ---- TEST 3: mylib1 + mylib2 (C11 redesigned libs) ---------------------
    trace_puts("\n== TEST 3: mylib1 + mylib2 (C11 test modules) ==\n");
    { const char* names[] = { "mylib1", "mylib2" };
      if (load_set_of(names, 2)) {
        auto library_function  = fn<u32_fn1>("library_function");
        auto library_function2 = fn<u32_fn1>("library_function2");
        auto library_function3 = fn<u32_fn1>("library_function3");
        auto g                 = fn<u32_fn1>("g");
        auto fff               = fn<u32_fn1>("fff");
        auto boo               = fn<u32_fn0>("boo");
        auto boo2              = fn<u32_fn0>("boo2");
        auto foo               = fn<u32_fn0>("foo");
        auto lkj               = fn<u32_fn0>("lkj");
        auto callback_f        = fn<u32_fn1>("callback_f");
        auto retstr            = fn<char* (*)()>("retstr");
        auto retstr_           = fn<char* (*)()>("retstr_");
        
        auto lf                = fn<u32_fn1>("lf");
        auto lf2               = fn<u32_fn1>("lf2");
        auto lf3               = fn<u32_fn1>("lf3");
        auto xxx               = fn<u32_fn0>("xxx");

        // mylib1 checks
        std::uint32_t val_lf1 = library_function ? library_function(42) : 0;
        trace_u32("  library_function(42) = ", val_lf1);
        check("library_function(42) == 65", val_lf1 == 65u);

        std::uint32_t val_lf2 = library_function2 ? library_function2(42) : 0;
        trace_u32("  library_function2(42) = ", val_lf2);
        check("library_function2(42) == 128", val_lf2 == 128u);

        std::uint32_t val_lf3 = library_function3 ? library_function3(3) : 0;
        trace_u32("  library_function3(3) = ", val_lf3);
        check("library_function3(3) == 116", val_lf3 == 116u);

        std::uint32_t val_g = g ? g(5) : 0;
        trace_u32("  g(5) = ", val_g);
        check("g(5) == 70", val_g == 70u);

        std::uint32_t val_fff = fff ? fff(7) : 0;
        trace_u32("  fff(7) = ", val_fff);
        check("fff(7) == 67", val_fff == 67u);

        std::uint32_t val_boo = boo ? boo() : 0;
        check("boo() == 30", val_boo == 30u);

        std::uint32_t val_boo2 = boo2 ? boo2() : 0;
        check("boo2() == 60", val_boo2 == 60u);

        std::uint32_t val_foo = foo ? foo() : 0;
        check("foo() == 20", val_foo == 20u);

        std::uint32_t val_lkj = lkj ? lkj() : 0;
        check("lkj() == 155", val_lkj == 155u);

        std::uint32_t val_cf = callback_f ? callback_f(7) : 0;
        trace_u32("  callback_f(7) = ", val_cf);
        check("callback_f(7) == 221", val_cf == 221u);

        char* s_retstr = retstr ? retstr() : nullptr;
        char* s_retstr_ = retstr_ ? retstr_() : nullptr;
        check("retstr matches", s_retstr && std::string_view(s_retstr) == "Hello mylib1!");
        check("retstr_ matches", s_retstr_ && std::string_view(s_retstr_) == "Hello from mylib1!");

        // mylib2 checks
        std::uint32_t val_mylib2_lf = lf ? lf(42) : 0;
        trace_u32("  lf(42) = ", val_mylib2_lf);
        check("lf(42) == 65", val_mylib2_lf == 65u);

        std::uint32_t val_mylib2_lf2 = lf2 ? lf2(42) : 0;
        trace_u32("  lf2(42) = ", val_mylib2_lf2);
        check("lf2(42) == 128", val_mylib2_lf2 == 128u);

        std::uint32_t val_mylib2_lf3 = lf3 ? lf3(3) : 0;
        trace_u32("  lf3(3) = ", val_mylib2_lf3);
        check("lf3(3) == 90", val_mylib2_lf3 == 90u);

        std::uint32_t val_xxx = xxx ? xxx() : 0;
        trace_u32("  xxx() = ", val_xxx);
        check("xxx() == 60", val_xxx == 60u);

      } else check("load mylib1+mylib2", false);
    }

    // ---- summary ------------------------------------------------------------
    trace_puts("\n==== BATTERY SUMMARY: ");
    trace_u32("", static_cast<std::uint32_t>(g_pass));
    trace_puts("  passed, ");
    trace_u32("", static_cast<std::uint32_t>(g_fail));
    trace_puts("  failed ====\n");
    trace_puts(g_fail == 0 ? "RESULT: ALL PASS\n" : "RESULT: SOME FAILED\n");
    for (;;);
}

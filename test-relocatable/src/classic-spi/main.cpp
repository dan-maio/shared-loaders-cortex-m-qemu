/**
 * @file main.cpp
 * @brief Classic test suite loading .rel.o modules from SPI flash (W25Q64).
 */
#include "board.h"
#include "semihost.h"
#include "stm32f4xx_hal.h"
#include "reloc/reloc-loader.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <cstring>

/* W25Q64 SPI flash driver (HAL-based) */
#include "w25q64.h"
#include "fs-archive-spi.hpp"
#include "spi-flash.h"

/* SPI flash instance */
static w25q64 flash;

/* Semihosting helpers for provisioning */
static long flen(const char* path) {
    int h = semihost_open(path, 1);
    if (h < 0) return -1;
    long n = semihost_flen(h);
    semihost_close(h);
    return n;
}

static bool read_file(const char* path, std::byte* buf, long sz) {
    int h = semihost_open(path, 1);
    if (h < 0) return false;
    bool ok = (semihost_read(h, buf, (int)sz) == 0);
    semihost_close(h);
    return ok;
}

/* SPI flash C API (called by fs-archive-spi.cpp) */
extern "C" {
void spi_flash_sector_erase(uint32_t addr) { flash.sector_erase(addr); }
int  spi_flash_page_program(uint32_t addr, const uint8_t* data, uint16_t len) { return flash.page_program(addr, data, len) ? 0 : -1; }
int  spi_flash_write(uint32_t addr, const uint8_t* data, uint32_t len) { return flash.write(addr, data, len) ? 0 : -1; }
void spi_flash_read(uint32_t addr, uint8_t* data, uint16_t len) { flash.read(addr, data, len); }
void spi_flash_get_sr(uint32_t* sr, uint32_t* cr1) { (void)sr; (void)cr1; }
}

/* .rel.o fixture files to provision */
#define RELOC_COUNT 9
static const char* reloc_names[RELOC_COUNT] = {
    "relprov", "relmod", "c1", "c2", "cpp3",
    "libprov", "libcons", "mylib1", "mylib2"
};

/* RAM arena for loaded modules (same as classic) */
alignas(8) std::byte g_arena[8 * 1024];
alignas(8) std::byte g_arena2[8 * 1024];

/* Loaded module sets */
reloc::module_set g_set;
reloc::module_set g_set2;

/* Image buffers for loading */
alignas(8) std::byte g_img[3][8 * 1024];
std::span<const std::byte> g_spans[3];

/* Reloc fixtures path */
#ifndef RELOC_LIB_DIR
#define RELOC_LIB_DIR "."
#endif

/* ── trace helpers (same as classic) ──────────────────────────────────────── */
extern "C" {
void trace_u32(const char* label, std::uint32_t v) {
    trace_puts(label);
    char b[10]; int i = 0;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_putc('\n');
}
void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_putc('['); trace_puts(origin); trace_puts("] "); trace_puts(fn); trace_putc('(');
    char b[10]; int i = 0; std::uint32_t v = arg;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_puts(")\n");
}
void trace_u32_raw(std::uint32_t v) {
    char b[10]; int i = 0;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
}
void trace_printf(const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 'u' || *format == 'd' || *format == 'x' || *format == 's' || *format == 'l') {
                char ch = *format;
                if (ch == 'l') { format++; ch = *format; }
                if (ch == 'u' || ch == 'd') {
                    trace_u32_raw(__builtin_va_arg(args, unsigned int));
                } else if (ch == 'x') {
                    unsigned int val = __builtin_va_arg(args, unsigned int);
                    static const char h[] = "0123456789abcdef";
                    trace_puts("0x");
                    char buf[8]; int idx = 0;
                    if (!val) trace_putc('0'); else { while (val) { buf[idx++] = h[val % 16]; val /= 16; } while (idx) trace_putc(buf[--idx]); }
                } else if (ch == 's') {
                    const char* s = __builtin_va_arg(args, const char*);
                    if (s) trace_puts(s); else trace_puts("(null)");
                }
                format++;
            } else { trace_putc('%'); trace_putc(*format++); }
        } else { trace_putc(*format++); }
    }
    __builtin_va_end(args);
}
}

#define log_info(fmt, ...)  do { trace_printf("[INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define log_error(fmt, ...) do { trace_printf("[ERR] " fmt "\n", ##__VA_ARGS__); } while(0)

/* ── Symbols exported to loaded modules (same as classic) ─────────────────── */
extern "C" {
std::uint32_t AA = 5;  std::uint32_t BB = 55;
const char* s1 = "Help....\n";
std::uint32_t f(std::uint32_t x) { return 3 * x; }
std::uint32_t w(std::uint32_t x) { return AA + x; }
float __aeabi_d2f(double); double __aeabi_f2d(float);
float sinf(float); float cosf(float); float logf(float); float sqrtf(float);
void* malloc(std::size_t); void free(void*);
int* __errno(void) { static int e; return &e; }
std::uint32_t fm_var = 170u;
struct FmInfo { std::uint32_t id, flags; } fm_info = { 0xF1u, 7u };
std::uint32_t fm_callback(std::uint32_t x) { trace_event("FM", "fm_callback", x); return x + 0x100u; }
int printf(const char* format, ...) {
    __builtin_va_list args; __builtin_va_start(args, format);
    trace_puts("[printf] ");
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 'u') { trace_u32_raw(__builtin_va_arg(args, unsigned int)); format++; }
            else if (*format == 's') { const char* s = __builtin_va_arg(args, const char*); trace_puts(s ? s : "(null)"); format++; }
            else { trace_putc('%'); trace_putc(*format++); }
        } else { trace_putc(*format++); }
    }
    __builtin_va_end(args); return 0;
}
char* strcat(char* dest, const char* src) { char* d = dest; while (*d) d++; while ((*d++ = *src++)); return dest; }
double log(double x) { return static_cast<double>(logf(static_cast<float>(x))); }
unsigned int lkj() { return 56; } unsigned int mm() { return AA + AA; }
}

/* ── Resolver (same as classic) ───────────────────────────────────────────── */
namespace {
template <class F> std::uint32_t addr(F f) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(f)); }

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
    if (n == "lkj")         return addr(&lkj);
    if (n == "mm")          return addr(&mm);
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

/* Build path: RELOC_LIB_DIR/<name>.rel.o */
template <std::size_t N>
void make_path(char (&path)[N], const char* name) {
    // N is deduced from the caller's array, so the bound cannot disagree with
    // the buffer. Previously this took a bare char* and wrote until the inputs
    // ran out; RELOC_LIB_DIR is a build-time define, and a long enough one
    // overran every caller's path[] and corrupted the stack.
    std::size_t k = 0;
    for (const char* p = RELOC_LIB_DIR "/"; *p && k + 1 < N; ++p) path[k++] = *p;
    for (const char* p = name; *p && k + 1 < N; ++p) path[k++] = *p;
    for (const char* p = ".rel.o"; *p && k + 1 < N; ++p) path[k++] = *p;
    path[k] = 0;
}

/* ── Logger shim (same as classic) ─────────────────────────────────────────── */
namespace elf_loader {
namespace log {
inline bool g_verbose = true;
inline void set_verbose(bool v) { g_verbose = v; }
}
}

/* Load one module set (multi-span) from SPI flash into arena */
bool load_set_of(const char* const* names, int n) {
    if (n > 3) return false;
    for (int i = 0; i < n; ++i) {
        const fw::FsEntry* e = fw::fs_find(names[i]);
        if (!e) { trace_puts("  fs_find FAILED: "); trace_puts(names[i]); trace_putc('\n'); return false; }
        flash.read(e->off, (uint8_t*)g_img[i], e->size);
        g_spans[i] = std::span<const std::byte>(g_img[i], e->size);
    }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(g_spans, n), std::span(g_arena), rs);
    if (!r) {
        trace_puts("  load_set FAILED: "); trace_u32_raw(static_cast<std::uint32_t>(r.err())); trace_putc('\n');
        return false;
    }
    g_set = *r;
    return true;
}

/* Load one module into a specific arena (for multi-instance tests) */
bool load_into(const char* name, int slot, std::span<std::byte> arena, reloc::module_set& out) {
    char path[256]; make_path(path, name);
    int h = semihost_open(path, 1);
    if (h < 0) return false;
    long n = semihost_flen(h); semihost_close(h);
    if (n <= 0 || (std::size_t)n > sizeof g_img[slot]) return false;
    /* For multi-instance tests, we still load from SPI flash via fs_find */
    const fw::FsEntry* e = fw::fs_find(name);
    if (!e) return false;
    flash.read(e->off, (uint8_t*)g_img[slot], e->size);
    std::span<const std::byte> imgs[1] = { std::span<const std::byte>(g_img[slot], (std::size_t)e->size) };
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(imgs, 1), arena, rs);
    if (!r) return false;
    out = *r;
    return true;
}

template <class F> F fn(std::string_view s) { return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(g_set, s))); }
template <class F> F fn_of(const reloc::module_set& set, std::string_view s) { return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(set, s))); }
} // namespace

struct str_lib { char *a; float f; char *bb; unsigned int b; unsigned int c; };
unsigned int (*library_function)(unsigned int) = nullptr;
unsigned int (*library_function2)(unsigned int) = nullptr;
unsigned int (*library_function3)(unsigned int) = nullptr;
unsigned int (*g)(unsigned int) = nullptr;
unsigned int (*fff)(unsigned int) = nullptr;
unsigned int (*gg)(unsigned int) = nullptr;
char* (*retstr)() = nullptr;
char* (*retstr_)() = nullptr;
struct str_lib (*ret_struct_lib)() = nullptr;
struct str_lib (*ret_struct)(struct str_lib) = nullptr;
unsigned int (*callback_f)(unsigned int) = nullptr;
unsigned int (*boo)() = nullptr;
unsigned int (*foo)() = nullptr;
unsigned int (*lf)(unsigned int) = nullptr;
unsigned int (*lf2)(unsigned int) = nullptr;
unsigned int (*lf3)(unsigned int) = nullptr;
unsigned int (*xxx)() = nullptr;
void (*pl)() = nullptr;
void (*pl2)(char*) = nullptr;

int main(void) {
    board_init();
    HAL_Init();
    trace_puts("Hello Arm World! (SPI flash)\n");

    /* ── Init SPI flash and format filesystem ──────────────────────────── */
    flash.power_up();
    trace_puts("SPI flash init OK\n");

    trace_puts("Formatting SPI flatfs...\n");
    if (!fw::fs_format()) { log_error("fs_format failed"); for (;;); }

    /* ── Provision .rel.o files to SPI flash (read from semihosting) ──── */
    std::byte rbuf[16 * 1024];
    int count = 0;
    for (int i = 0; i < RELOC_COUNT; ++i) {
        char path[256]; make_path(path, reloc_names[i]);
        long sz = flen(path);
        if (sz <= 0 || (std::size_t)sz > sizeof rbuf) {
            log_error("skip %s (sz=%ld)", reloc_names[i], sz); continue;
        }
        if (!read_file(path, rbuf, sz)) {
            log_error("read FAILED: %s", reloc_names[i]); continue;
        }
        if (!fw::fs_add(reloc_names[i], rbuf, (std::uint32_t)sz)) {
            log_error("fs_add FAILED: %s", reloc_names[i]); continue;
        }
        log_info("saved %s (%ld B)", reloc_names[i], sz);
        count++;
    }
    log_info("provisioned %d modules to SPI flash", count);

    /* ── Relocatable loader tests (identical to classic, but from SPI) ── */
    elf_loader::log::set_verbose(true);

    /* relprov + relmod */
    trace_puts("\n== relprov + relmod (cross-module ABS32) ==\n");
    { const char* names[] = { "relprov", "relmod" };
      if (load_set_of(names, 2)) {
        auto rel_compute   = fn<unsigned int (*)(unsigned int)>("rel_compute");
        auto rel_read_data = fn<unsigned int (*)()>("rel_read_data");
        auto prov_state    = fn<unsigned int (*)()>("prov_state");
        std::uint32_t r = rel_compute ? rel_compute(100) : 0;
        log_info("rel_compute(100)=%u  rel_read_data=%x  prov_state=%u",
                 r, rel_read_data ? rel_read_data() : 0, prov_state ? prov_state() : 0);
      } else log_error("load relprov+relmod failed");
    }

    /* c1 + c2 + cpp3 */
    trace_puts("\n== c1 + c2 + cpp3 (cyclic set, load_set) ==\n");
    { const char* names[] = { "c1", "c2", "cpp3" };
      if (load_set_of(names, 3)) {
        auto c1_call_c2 = fn<unsigned int (*)(unsigned int)>("c1_call_c2");
        auto c1_state   = fn<unsigned int (*)()>("c1_state");
        auto c1_use_fm  = fn<unsigned int (*)()>("c1_use_fm");
        auto cpp3_recurse = fn<unsigned int (*)(unsigned int)>("cpp3_recurse");
        auto cpp3_method  = fn<unsigned int (*)(unsigned int, unsigned int)>("cpp3_method");
        std::uint32_t a = c1_call_c2 ? c1_call_c2(5) : 0;
        std::uint32_t b = c1_call_c2 ? c1_call_c2(5) : 0;
        log_info("c1_call_c2(5)=%u (2nd=%u) c1_state=%u", a, b, c1_state ? c1_state() : 0);
        log_info("c1_use_fm()=%u", c1_use_fm ? c1_use_fm() : 0);
        log_info("cpp3_recurse(6)=%u", cpp3_recurse ? cpp3_recurse(6) : 0);
        std::uint32_t m1 = cpp3_method ? cpp3_method(2, 100) : 0;
        std::uint32_t m2 = cpp3_method ? cpp3_method(2, 100) : 0;
        log_info("cpp3_method(2,100)=%u (2nd=%u)", m1, m2);
      } else log_error("load_set c1+c2+cpp3 failed");
    }

    /* libprov + libcons */
    trace_puts("\n== libprov + libcons ==\n");
    { const char* names[] = { "libprov", "libcons" };
      if (load_set_of(names, 2)) {
        auto cons_call = fn<unsigned int (*)(unsigned int)>("cons_call");
        log_info("cons_call(10)=%u", cons_call ? cons_call(10) : 0);
      } else log_error("load libprov+libcons failed");
    }

    /* mylib1 + mylib2 */
    trace_puts("\n== mylib1 + mylib2 ==\n");
    { const char* names[] = { "mylib1", "mylib2" };
      if (!load_set_of(names, 2)) { log_error("load mylib1+mylib2 failed"); for (;;); }
    }

    /* Resolve function pointers */
    library_function  = fn<decltype(library_function)>("library_function");
    library_function2 = fn<decltype(library_function2)>("library_function2");
    library_function3 = fn<decltype(library_function3)>("library_function3");
    g  = fn<decltype(g)>("g"); fff = fn<decltype(fff)>("fff"); gg = fn<decltype(gg)>("gg");
    retstr = fn<decltype(retstr)>("retstr"); retstr_ = fn<decltype(retstr_)>("retstr_");
    ret_struct_lib = fn<decltype(ret_struct_lib)>("ret_struct_lib");
    ret_struct = fn<decltype(ret_struct)>("ret_struct");
    callback_f = fn<decltype(callback_f)>("callback_f");
    boo = fn<decltype(boo)>("boo"); foo = fn<decltype(foo)>("foo");
    lf = fn<decltype(lf)>("lf"); lf2 = fn<decltype(lf2)>("lf2"); lf3 = fn<decltype(lf3)>("lf3");
    xxx = fn<decltype(xxx)>("xxx"); pl = fn<decltype(pl)>("pl"); pl2 = fn<decltype(pl2)>("pl2");

    auto* p_aa = (std::uint32_t*)reloc::sym(g_set, "aa");
    auto* p_bb = (std::uint32_t*)reloc::sym(g_set, "bb");
    auto* p_aaa = (std::uint32_t*)reloc::sym(g_set, "aaa");
    auto* p_bbb = (std::uint32_t*)reloc::sym(g_set, "bbb");

    unsigned int z = library_function(7);
    auto w0 = library_function2(7); auto q = library_function3(7); *p_aa = 20;
    log_info("library_function(7)=%u lib2(7)=%u lib3(7)=%u", z, w0, q);
    log_info("aa=%lu bb=%lu aaa=%lu bbb=%lu", (unsigned long)*p_aa, (unsigned long)*p_bb, (unsigned long)*p_aaa, (unsigned long)*p_bbb);
    log_info("g(7)=%u fff(10)=%u gg(7)=%u", g(7), fff(10), gg(7));
    log_info("retstr=%s retstr_=%s", retstr(), retstr_());

    char sl_a[32] = "1000"; char sl_bb[32] = "xxx";
    struct str_lib sl = {sl_a, 2.01256f, sl_bb, 2, 3};
    auto sl1 = ret_struct_lib();
    auto sll = ret_struct(sl);
    log_info("Struct ret: %s %d %s %f", sll.a, sll.b, sll.bb, double(sll.f));
    auto qq = callback_f(7);
    log_info("callback_f(7)=%u fff(10)=%u", qq, fff(10));
    char toto[] = "TOTO"; pl2(toto);
    log_info("xxx()=%u", xxx());

    trace_puts("\n==== classic-spi demo complete ====\n");
    for (;;);
}

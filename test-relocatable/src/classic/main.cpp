/**
 * @file main.cpp
 * @brief Classic static test suite updated for the scl::reloc relocatable loader.
 */
#include "board.h"
#include "semihost.h"
#include "reloc/reloc-loader.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// flatfs: fixture modules linked into FLASH as const arrays (embed-blob.py).
// Loaded straight from their flash address — no semihosting file I/O.
extern "C" {
extern const unsigned char fsmod1_rel_o[]; extern const unsigned fsmod1_rel_o_len;
extern const unsigned char fsmod2_rel_o[]; extern const unsigned fsmod2_rel_o_len;
// A *real* flat-fs archive: fsmod1+fsmod2 packed into ONE flash blob with a
// directory (pack-flatfs.py). Mounted at runtime; modules opened BY NAME.
extern const unsigned char flatfs_archive[]; extern const unsigned flatfs_archive_len;
}

// ── Simple Led/Timer shims to replace STM32 HAL dependencies ───────────────
class led {
public:
    led(unsigned int port, unsigned int bit, bool activeLow) {}
    void power_up() {}
    void turn_on() { led_set(1); }
    void turn_off() { led_set(0); }
    void toggle() { led_toggle(); }
};

class timer {
public:
    using ticks_t = uint32_t;
    static constexpr ticks_t FREQUENCY_HZ = 1000;
    void start() {}
    void sleep(ticks_t ticks) { delay_ms(ticks); }
};

namespace {
constexpr timer::ticks_t BLINK_ON_TICKS  = timer::FREQUENCY_HZ * 3 / 4;
constexpr timer::ticks_t BLINK_OFF_TICKS = timer::FREQUENCY_HZ - BLINK_ON_TICKS;
led blink_leds[1] = {{0, 5, false}};
}

// ── Simple trace_printf formatter ──────────────────────────────────────────
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

void trace_u32_raw(std::uint32_t v) {
    char b[10]; int i = 0;
    if (!v) trace_putc('0');
    else {
        while (v) { b[i++] = char('0' + v % 10); v /= 10; }
        while (i) trace_putc(b[--i]);
    }
}

void trace_printf(const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 'u' || *format == 'd' || *format == 'x' || *format == 's' || *format == 'l') {
                char ch = *format;
                if (ch == 'l') {
                    format++; // skip 'l' (e.g. in %lu)
                    ch = *format;
                }
                if (ch == 'u' || ch == 'd') {
                    unsigned int val = __builtin_va_arg(args, unsigned int);
                    trace_u32_raw(val);
                } else if (ch == 'x') {
                    unsigned int val = __builtin_va_arg(args, unsigned int);
                    static const char h[] = "0123456789abcdef";
                    trace_puts("0x");
                    char buf[8]; int idx = 0;
                    if (!val) trace_putc('0');
                    else {
                        while (val) { buf[idx++] = h[val % 16]; val /= 16; }
                        while (idx) trace_putc(buf[--idx]);
                    }
                } else if (ch == 's') {
                    const char* s = __builtin_va_arg(args, const char*);
                    if (s) trace_puts(s);
                    else trace_puts("(null)");
                }
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
}
}

// ── Simple logger API to match logger.h ────────────────────────────────────
namespace elf_loader {
namespace log {
inline bool g_verbose = true;
inline void set_verbose(bool v) { g_verbose = v; }
inline const char* hex(std::uint32_t v) {
    static char buf[11];
    static const char h[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; ++i)
        buf[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    return buf;
}
inline const char* hex16(std::uint16_t v) {
    static char buf[7];
    static const char h[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 4; ++i)
        buf[2 + i] = h[(v >> ((3 - i) * 4)) & 0xF];
    buf[6] = '\0';
    return buf;
}
inline const char* binding_str(std::uint8_t b) {
    switch (b) {
        case 0: return "LOCAL";
        case 1: return "GLOBAL";
        case 2: return "WEAK";
        default: return "?";
    }
}
} // namespace log
} // namespace elf_loader

#define log_info(fmt, ...)  do { trace_printf("[INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define log_error(fmt, ...) do { trace_printf("[ERR] " fmt "\n", ##__VA_ARGS__); } while(0)

// ── Symbols exported to the shared libraries ──────────────────────────────
extern "C" {
std::uint32_t AA = 5;
std::uint32_t BB = 55;
const char* s1 = "Help....\n";
std::uint32_t f(std::uint32_t x) { return 3 * x; }
std::uint32_t w(std::uint32_t x) { return AA + x; }

float __aeabi_d2f(double);
double __aeabi_f2d(float);

float sinf(float); float cosf(float); float logf(float); float sqrtf(float);
void* malloc(std::size_t); void free(void*);
int* __errno(void) { static int e; return &e; }

std::uint32_t          fm_var  = 170u;
const struct FmInfo { std::uint32_t id, flags; } fm_info = { 0xF1u, 7u };
std::uint32_t fm_callback(std::uint32_t x) { trace_event("FM", "fm_callback", x); return x + 0x100u; }

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

unsigned int lkj() { return 56; }
unsigned int mm() { return AA + AA; }
}

namespace {

template <class F> std::uint32_t addr(F f) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(f));
}

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

alignas(8) std::byte g_img[3][8 * 1024];
alignas(8) std::byte g_arena[8 * 1024];
alignas(8) std::byte g_arena2[8 * 1024];   // 2nd arena: independent-instance test
std::span<const std::byte> g_spans[3];
reloc::module_set g_set;
reloc::module_set g_set2;

// Build "<RELOC_LIB_DIR>/<name>.rel.o" into path[].
void make_path(char* path, const char* name) {
    int k = 0;
    for (const char* p = RELOC_LIB_DIR "/"; *p; ++p) path[k++] = *p;
    for (const char* p = name; *p; ++p) path[k++] = *p;
    for (const char* p = ".rel.o"; *p; ++p) path[k++] = *p;
    path[k] = 0;
}

// Load ONE module (read into g_img[slot]) into @p arena -> @p out. Used to place
// the same module into two different arenas, and to reload into the same one.
bool load_into(const char* name, int slot, std::span<std::byte> arena, reloc::module_set& out) {
    char path[128]; make_path(path, name);
    std::size_t sz = read_file(path, g_img[slot], sizeof g_img[slot]);
    if (!sz) return false;
    std::span<const std::byte> imgs[1] = { std::span<const std::byte>(g_img[slot], sz) };
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(imgs, 1), arena, rs);
    if (!r) return false;
    out = *r;
    return true;
}

bool load_set_of(const char* const* names, int n) {
    if (n > 3) return false;
    for (int i = 0; i < n; ++i) {
        char path[128]; int k = 0;
        for (const char* p = RELOC_LIB_DIR "/"; *p; ++p) path[k++] = *p;
        for (const char* p = names[i]; *p; ++p) path[k++] = *p;
        for (const char* p = ".rel.o"; *p; ++p) path[k++] = *p;
        path[k] = 0;
        std::size_t sz = read_file(path, g_img[i], sizeof g_img[i]);
        if (!sz) { trace_puts("  read FAILED: "); trace_puts(path); trace_putc('\n'); return false; }
        g_spans[i] = std::span<const std::byte>(g_img[i], sz);
    }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(g_spans, n), std::span(g_arena), rs);
    if (!r) {
        trace_puts("  load_set FAILED with error: ");
        trace_u32_raw(static_cast<std::uint32_t>(r.err()));
        trace_putc('\n');
        return false;
    }
    g_set = *r;
    return true;
}

template <class F> F fn(std::string_view s) {
    return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(g_set, s)));
}
template <class F> F fn_of(const reloc::module_set& set, std::string_view s) {
    return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(set, s)));
}

// ── flatfs: a minimal read-only flat filesystem living in FLASH ────────────
// Archive layout (pack-flatfs.py): "FLFS" | u32 count | count*{name[16], u32
// off, u32 size} | data. Because flash is memory-mapped, mount() just records
// the base pointer and open() returns a span straight into flash — zero copy.
struct flatfs {
    const unsigned char* base = nullptr;
    unsigned             len  = 0;
    static std::uint32_t u32(const unsigned char* p) {
        return (std::uint32_t)p[0] | (std::uint32_t)p[1] << 8 |
               (std::uint32_t)p[2] << 16 | (std::uint32_t)p[3] << 24;
    }
    bool mount(const unsigned char* p, unsigned n) {
        if (n < 8 || p[0] != 'F' || p[1] != 'L' || p[2] != 'F' || p[3] != 'S')
            return false;
        base = p; len = n; return true;
    }
    unsigned count() const { return u32(base + 4); }
    // Look up an entry by name; returns an empty span if not found.
    std::span<const std::byte> open(std::string_view name) const {
        for (unsigned i = 0, c = count(); i < c; ++i) {
            const unsigned char* e = base + 8 + i * 24;      // name[16] + 2*u32
            unsigned nl = 0; while (nl < 16 && e[nl]) ++nl;
            std::string_view en((const char*)e, nl);
            if (en == name)
                return std::span<const std::byte>(
                    (const std::byte*)(base + u32(e + 16)), u32(e + 20));
        }
        return {};
    }
};

} // namespace

// Struct format matching mylib1.h
struct str_lib {
    char         *a;
    float         f;
    char         *bb;
    unsigned int  b;
    unsigned int  c;
};

// ── Function pointers for library entry points ─────────────────────────────
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
    elf_loader::log::set_verbose(true);
    trace_puts("Hello Arm World!\n");

    // ---- relprov + relmod (acyclic: relmod imports prov_add) --------
    trace_puts("\n== relprov + relmod (cross-module ABS32) ==\n");
    { const char* names[] = { "relprov", "relmod" };
      if (load_set_of(names, 2)) {
        auto rel_compute   = fn<unsigned int (*)(unsigned int)>("rel_compute");
        auto rel_read_data = fn<unsigned int (*)()>("rel_read_data");
        auto prov_state    = fn<unsigned int (*)()>("prov_state");
        std::uint32_t r = rel_compute ? rel_compute(100) : 0;
        log_info("rel_compute(100)=%u  rel_read_data=%x  prov_state=%u",
                 r,
                 rel_read_data ? rel_read_data() : 0,
                 prov_state ? prov_state() : 0);
      } else log_error("load relprov+relmod failed");
    }

    // ---- c1 + c2 + cpp3 (CYCLIC: must be one load_set) --------------
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
        log_info("c1_call_c2(5)=%u  (2nd call=%u, .bss persists)  c1_state=%u",
                 a, b, c1_state ? c1_state() : 0);

        std::uint32_t u = c1_use_fm ? c1_use_fm() : 0;
        log_info("c1_use_fm()=%u (reads FM data)", u);

        std::uint32_t d = cpp3_recurse ? cpp3_recurse(6) : 0;
        log_info("cpp3_recurse(6)=%u", d);

        std::uint32_t m1 = cpp3_method ? cpp3_method(2, 100) : 0;
        std::uint32_t m2 = cpp3_method ? cpp3_method(2, 100) : 0;
        log_info("cpp3_method(2,100)=%u (2nd call=%u, libm)", m1, m2);
      } else log_error("load_set c1+c2+cpp3 failed");
    }

    // ---- libprov + libcons --------------------------------------------
    trace_puts("\n== libprov + libcons ==\n");
    { const char* names[] = { "libprov", "libcons" };
      if (load_set_of(names, 2)) {
        auto cons_call = fn<unsigned int (*)(unsigned int)>("cons_call");
        std::uint32_t r = cons_call ? cons_call(10) : 0;
        log_info("cons_call(10)=%u", r);
      } else log_error("load libprov+libcons failed");
    }

    // ---- multi — one .rel.o bundled from 3 .cpp via `ld -r` ------------
    // multi_a -> multi_b -> multi_c (all resolved inside the bundle by ld -r);
    // only trace_u32 stays a host import. Exercises both a cross-file FUNCTION
    // call chain and a cross-file shared VARIABLE (multi_shared).
    trace_puts("\n== multi (3-file ld -r bundle) ==\n");
    { const char* names[] = { "multi" };
      if (load_set_of(names, 1)) {
        auto multi_entry      = fn<unsigned int (*)(unsigned int)>("multi_entry");
        auto multi_get_shared = fn<unsigned int (*)()>("multi_get_shared");
        auto* p_shared        = (std::uint32_t*)reloc::sym(g_set, "multi_shared");
        std::uint32_t e = multi_entry ? multi_entry(5) : 0;   // a->b->c + shared
        log_info("multi_entry(5)=%u (a->b->c across 3 files)  get_shared()=%u  shared-as-data=%u",
                 e,
                 multi_get_shared ? multi_get_shared() : 0,
                 p_shared ? *p_shared : 0);
      } else log_error("load multi bundle failed");
    }

    // ---- two independent instances (same .rel.o, two arenas) --------
    // -fno-pic modules are bound to one address, so a 2nd copy is re-relocated
    // into its own arena. The two instances have SEPARATE .bss at DIFFERENT
    // absolute addresses — impossible to conflate.
    trace_puts("\n== two independent instances (same module, 2 arenas) ==\n");
    if (load_into("multi", 0, std::span(g_arena),  g_set) &&
        load_into("multi", 1, std::span(g_arena2), g_set2)) {
        auto bumpA = fn_of<unsigned int (*)()>(g_set,  "multi_bump");
        auto bumpB = fn_of<unsigned int (*)()>(g_set2, "multi_bump");
        auto getA  = fn_of<unsigned int (*)()>(g_set,  "multi_hits_get");
        auto getB  = fn_of<unsigned int (*)()>(g_set2, "multi_hits_get");
        std::uint32_t addrA = reloc::sym(g_set,  "multi_shared");
        std::uint32_t addrB = reloc::sym(g_set2, "multi_shared");
        bumpA(); bumpA();     // instance A: 2 hits
        bumpB();              // instance B: 1 hit
        log_info("A@%x hits=%u   B@%x hits=%u (independent .bss)",
                 addrA, getA(), addrB, getB());
    } else log_error("load two multi instances failed");

    // ---- reload resets .bss + re-runs the constructor ---------------
    trace_puts("\n== reload resets .bss/ctor state ==\n");
    if (load_into("multi", 0, std::span(g_arena), g_set)) {
        auto bump = fn_of<unsigned int (*)()>(g_set, "multi_bump");
        bump(); bump(); bump();                       // hits -> 3
        std::uint32_t before = fn_of<unsigned int (*)()>(g_set, "multi_hits_get")();
        load_into("multi", 0, std::span(g_arena), g_set);   // reload = fresh instance
        std::uint32_t after  = fn_of<unsigned int (*)()>(g_set, "multi_hits_get")();
        std::uint32_t shared = fn_of<unsigned int (*)()>(g_set, "multi_get_shared")();
        log_info("hits before reload=%u  after=%u (.bss cleared)  shared=%u (ctor re-ran)",
                 before, after, shared);
    } else log_error("reload multi failed");

    // ---- loader error contract (negative paths, must NOT crash) -----
    trace_puts("\n== loader error contract (negative paths) ==\n");
    {
        // (a) unresolved import: cpp3 imports c2_* which nothing here provides
        const char* solo[] = { "cpp3" };
        log_info("unresolved import -> load %s", load_set_of(solo, 1) ? "OK (unexpected)" : "fails (no crash)");

        loader::symbol_source rs{ main_resolve, nullptr };

        // (b) a non-ELF garbage buffer -> NOT_ELF
        static const std::byte junk[64] = {};
        std::span<const std::byte> gi[1] = { std::span<const std::byte>(junk, sizeof junk) };
        auto rg = reloc::load_set(std::span<const std::span<const std::byte>>(gi, 1), std::span(g_arena), rs);
        log_info("garbage buffer -> err=%d (NOT_ELF=%d)",
                 rg ? -1 : (int)rg.err(), (int)loader::error::NOT_ELF);

        // (c) a real module into a 64-byte arena -> OUT_OF_MEMORY
        char path[128]; make_path(path, "mylib1");
        std::size_t sz = read_file(path, g_img[0], sizeof g_img[0]);
        std::span<const std::byte> ti[1] = { std::span<const std::byte>(g_img[0], sz) };
        static std::byte tiny[64];
        auto rt = reloc::load_set(std::span<const std::span<const std::byte>>(ti, 1), std::span(tiny), rs);
        log_info("tiny arena -> err=%d (OUT_OF_MEMORY=%d)",
                 rt ? -1 : (int)rt.err(), (int)loader::error::OUT_OF_MEMORY);
    }

    trace_puts("\n== mylib1 + mylib2 ==\n");
    const char* names[] = { "mylib1", "mylib2" };
    if (!load_set_of(names, 2)) {
        log_error("failed to load relocatable libraries");
        for (;;);
    }

    // Resolve pointers
    library_function  = fn<decltype(library_function)>("library_function");
    library_function2 = fn<decltype(library_function2)>("library_function2");
    library_function3 = fn<decltype(library_function3)>("library_function3");
    g                 = fn<decltype(g)>("g");
    fff               = fn<decltype(fff)>("fff");
    gg                = fn<decltype(gg)>("gg");
    retstr            = fn<decltype(retstr)>("retstr");
    retstr_           = fn<decltype(retstr_)>("retstr_");
    ret_struct_lib    = fn<decltype(ret_struct_lib)>("ret_struct_lib");
    ret_struct        = fn<decltype(ret_struct)>("ret_struct");
    callback_f        = fn<decltype(callback_f)>("callback_f");
    boo               = fn<decltype(boo)>("boo");
    foo               = fn<decltype(foo)>("foo");

    lf                = fn<decltype(lf)>("lf");
    lf2               = fn<decltype(lf2)>("lf2");
    lf3               = fn<decltype(lf3)>("lf3");
    xxx               = fn<decltype(xxx)>("xxx");
    pl                = fn<decltype(pl)>("pl");
    pl2               = fn<decltype(pl2)>("pl2");

    auto* p_aa = (std::uint32_t*)reloc::sym(g_set, "aa");
    auto* p_bb = (std::uint32_t*)reloc::sym(g_set, "bb");
    auto* p_aaa = (std::uint32_t*)reloc::sym(g_set, "aaa");
    auto* p_bbb = (std::uint32_t*)reloc::sym(g_set, "bbb");

    // ── Library calls from main ──────────────────────────────────────
    unsigned int z = library_function(7);
    auto w0 = library_function2(7);
    auto q  = library_function3(7);
    *p_aa = 20;

    log_info("library_function(7)=%u  lib2(7)=%u  lib3(7)=%u", z, w0, q);
    log_info("aa=%lu bb=%lu aaa=%lu bbb=%lu",
                          (unsigned long)*p_aa,
                          (unsigned long)*p_bb,
                          (unsigned long)*p_aaa,
                          (unsigned long)*p_bbb);
    log_info("g(7)=%u  fff(10)=%u  gg(7)=%u", g(7), fff(10), gg(7));
    log_info("retstr=%s  retstr_=%s", retstr(), retstr_());

    // ── ret_struct tests ─────────────────────────────────────────────
    char sl_a[32] = "1000";
    char sl_bb[32] = "xxx";
    struct str_lib sl = {sl_a, 2.01256f, sl_bb, 2, 3};
    log_info("Struct: %s %s %d %d %f", sl.a, sl.bb, sl.b, sl.c, double(sl.f));

    auto sl1 = ret_struct_lib();
    log_info("Struct from lib: %s %d %s %d %f",
                          sl1.a, sl1.b, sl1.bb, sl1.c, double(sl1.f));

    auto sll = ret_struct(sl);
    log_info("Struct ret: %s %d %s %d %f",
                          sll.a, sll.b, sll.bb, sll.c, double(sll.f));

    auto qq = callback_f(7);
    log_info("callback_f(7) = %u", qq);
    log_info("fff(10) = %u", fff(10));

    char toto[] = "TOTO";
    pl2(toto);
    log_info("xxx() = %u", xxx());

    // ── flatfs demo: load modules straight from FLASH (no semihosting FS) ────
    // fsmod1/fsmod2 are linked into the firmware's .rodata as const arrays, so a
    // module is just a span over its flash address. Classic/demo style — we load,
    // call, and log; no PASS/FAIL bookkeeping.
    trace_puts("\n== flatfs: modules loaded from FLASH ==\n");
    {
        loader::symbol_source rs{ main_resolve, nullptr };

        std::span<const std::byte> b1[1] = {
            std::span<const std::byte>((const std::byte*)fsmod1_rel_o, fsmod1_rel_o_len) };
        auto r1 = reloc::load_set(std::span<const std::span<const std::byte>>(b1, 1), std::span(g_arena), rs);
        if (r1) {
            g_set = *r1;
            auto id  = fn<unsigned int (*)()>("fsmod1_id");
            auto run = fn<unsigned int (*)(unsigned int)>("fsmod1_run");
            log_info("fsmod1 @flash(%u B): id=%x  run(5)=%u  run(5)=%u",
                     fsmod1_rel_o_len, id(), run(5), run(5));
        } else log_error("flatfs: fsmod1 load failed");

        std::span<const std::byte> b2[1] = {
            std::span<const std::byte>((const std::byte*)fsmod2_rel_o, fsmod2_rel_o_len) };
        auto r2 = reloc::load_set(std::span<const std::span<const std::byte>>(b2, 1), std::span(g_arena), rs);
        if (r2) {
            g_set = *r2;
            auto id = fn<unsigned int (*)()>("fsmod2_id");
            auto sq = fn<unsigned int (*)(unsigned int)>("fsmod2_square");
            log_info("fsmod2 @flash(%u B): id=%x  square(9)=%u", fsmod2_rel_o_len, id(), sq(9));
        } else log_error("flatfs: fsmod2 load failed");
    }

    // ── real flatfs demo: mount ONE flash archive, load modules BY NAME ──────
    // Unlike the block above (each module a separate linked array), here the two
    // .rel.o are packed into a single flash-resident archive with a directory.
    // We mount it and look up modules by name — the genuine "filesystem in
    // flash" shape. Classic/demo style — load, call, log; no PASS/FAIL.
    trace_puts("\n== flatfs: real archive mounted from FLASH (load by name) ==\n");
    {
        flatfs fs;
        if (!fs.mount(flatfs_archive, flatfs_archive_len)) {
            log_error("flatfs: bad archive magic");
        } else {
            log_info("mounted flatfs archive @flash(%u B): %u modules",
                     flatfs_archive_len, fs.count());
            loader::symbol_source rs{ main_resolve, nullptr };
            const char* names[] = { "fsmod1", "fsmod2" };
            for (const char* nm : names) {
                auto blob = fs.open(nm);
                if (blob.empty()) { log_error("flatfs: '%s' not found", nm); continue; }
                std::span<const std::byte> imgs[1] = { blob };
                auto r = reloc::load_set(
                    std::span<const std::span<const std::byte>>(imgs, 1),
                    std::span(g_arena), rs);
                if (!r) { log_error("flatfs: '%s' load failed", nm); continue; }
                g_set = *r;
                auto id = fn_of<unsigned int (*)()>(*r, nm[5] == '1' ? "fsmod1_id" : "fsmod2_id");
                log_info("opened '%s' (%u B) from archive: id=%x", nm,
                         (unsigned)blob.size(), id());
            }
        }
    }

    // ── RAM arena footprint: bytes each module places in the arena ───────────
    // module::used is the loader's own per-module counter (sum of the SHF_ALLOC
    // sections it copied). Printed here to confirm the measured ET_REL column in
    // the manual (§12.5) on real hardware. Modules load in sets (cyclic imports),
    // and g_set.mods[i] corresponds to names[i].
    trace_puts("\n== RAM arena footprint (module::used bytes) ==\n");
    {
        const char* s1[] = { "c1", "c2", "cpp3" };
        const char* s2[] = { "libprov", "libcons" };
        const char* s3[] = { "mylib1", "mylib2" };
        struct { const char* const* names; int n; } sets[] = {
            { s1, 3 }, { s2, 2 }, { s3, 2 },
        };
        for (auto& S : sets) {
            if (load_set_of(S.names, S.n))
                for (int i = 0; i < S.n; ++i)
                    log_info("%s arena=%u B", S.names[i], (unsigned)g_set.mods[i].used);
            else
                log_error("footprint: load %s... failed", S.names[0]);
        }
    }

    trace_puts("\n==== classic demo complete ====\n");

    // ── LED blink loop ───────────────────────────────────────────────
    timer tmr;
    tmr.start();

    for (auto& led : blink_leds) led.power_up();

    uint32_t seconds = 0;
    const int loops = 5;   // bare Reset_Handler calls main() with no args

    for (auto& led : blink_leds) led.turn_on();
    tmr.sleep(timer::FREQUENCY_HZ);
    for (auto& led : blink_leds) led.turn_off();
    tmr.sleep(BLINK_OFF_TICKS);
    ++seconds;
    trace_printf("Second %u\n", seconds);

    for (int idx = 0; idx < loops; ++idx) {
        blink_leds[0].turn_on();
        tmr.sleep(BLINK_ON_TICKS);
        blink_leds[0].turn_off();
        tmr.sleep(BLINK_OFF_TICKS);
        ++seconds;
        trace_printf("Second %u\n", seconds);
    }

    for (auto& led : blink_leds) led.turn_on();
    tmr.sleep(timer::FREQUENCY_HZ);

    semihost_exit(0);
    for (;;);
}

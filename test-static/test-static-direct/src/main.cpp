/**
 * @file main.cpp
 * @brief Combined demo application using direct linking and GOT/PLT patching via scl::static.
 */

#include <elf-reader.h>
#include <shlib-loader.h>
#include <static/module.h>
#include <logger.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "diag/trace.h"
#include "timer.h"
#include "led.h"
#include "mylib1.h"
#include "mylib2.h"
#include "cpp_features.h"
#include "tls_lib.h"

// ── Board-specific LED configuration ──────────────────────────────────────
namespace {
constexpr timer::ticks_t BLINK_ON_TICKS  = timer::FREQUENCY_HZ * 3 / 4;
constexpr timer::ticks_t BLINK_OFF_TICKS = timer::FREQUENCY_HZ - BLINK_ON_TICKS;
}

#if defined(STM32F401xE)
#warning "WeAct F411CE board, PC13, active high."
#define BLINK_PORT_NUMBER  (2)
#define BLINK_PIN_NUMBER   (13)
#define BLINK_ACTIVE_LOW   (false)
led blink_leds[1] = {{BLINK_PORT_NUMBER, BLINK_PIN_NUMBER, BLINK_ACTIVE_LOW}};

#elif defined(STM32F407xx)
#warning "Assume a STM32F4-Discovery board, PD12-PD15, active high."
#define BLINK_PORT_NUMBER        (3)
#define BLINK_PIN_NUMBER_GREEN   (12)
#define BLINK_PIN_NUMBER_ORANGE  (13)
#define BLINK_PIN_NUMBER_RED     (14)
#define BLINK_PIN_NUMBER_BLUE    (15)
#define BLINK_ACTIVE_LOW         (false)
led blink_leds[4] = {
    {BLINK_PORT_NUMBER, BLINK_PIN_NUMBER_GREEN,  BLINK_ACTIVE_LOW},
    {BLINK_PORT_NUMBER, BLINK_PIN_NUMBER_ORANGE, BLINK_ACTIVE_LOW},
    {BLINK_PORT_NUMBER, BLINK_PIN_NUMBER_RED,    BLINK_ACTIVE_LOW},
    {BLINK_PORT_NUMBER, BLINK_PIN_NUMBER_BLUE,   BLINK_ACTIVE_LOW},
};

#elif defined(STM32F411xE)
#if defined(SCL_BOARD_NUCLEO)
#warning "NUCLEO-F411RE board, LD2 on PA5, active high."
#define BLINK_PORT_NUMBER (0)
#define BLINK_PIN_NUMBER  (5)
#else
#warning "WeAct F411CE board, PC13, active high."
#define BLINK_PORT_NUMBER (2)
#define BLINK_PIN_NUMBER  (13)
#endif
#define BLINK_ACTIVE_LOW  (false)
led blink_leds[1] = {{BLINK_PORT_NUMBER, BLINK_PIN_NUMBER, BLINK_ACTIVE_LOW}};

#elif defined(STM32F429xx)
#warning "Assume a STM32F429I-Discovery board, PG13-PG14, active high."
#define BLINK_PORT_NUMBER      (6)
#define BLINK_PIN_NUMBER_GREEN (13)
#define BLINK_PIN_NUMBER_RED   (14)
#define BLINK_ACTIVE_LOW       (false)
led blink_leds[2] = {
    {BLINK_PORT_NUMBER, BLINK_PIN_NUMBER_GREEN, BLINK_ACTIVE_LOW},
    {BLINK_PORT_NUMBER, BLINK_PIN_NUMBER_RED,   BLINK_ACTIVE_LOW},
};
#else
#warning "WeAct F411CE board, PC13, active high."
#define BLINK_PORT_NUMBER (2)
#define BLINK_PIN_NUMBER  (13)
#define BLINK_ACTIVE_LOW  (false)
led blink_leds[1] = {{BLINK_PORT_NUMBER, BLINK_PIN_NUMBER, BLINK_ACTIVE_LOW}};
#endif

// malloc/free provided by newlib
extern "C" void* malloc(unsigned int);
extern "C" void free(void*);

// Local new/delete so the linker does NOT resolve _Znaj/_ZdlPv from
// libcpp_features.so's PLT — those operators must be callable before the .so
// is loaded (e.g. init_syms → symbol_table → new[]).
void* operator new(size_t size)                  { return malloc(size); }
void* operator new[](size_t size)                { return malloc(size); }
void  operator delete(void* p) noexcept           { if (p) free(p); }
void  operator delete[](void* p) noexcept         { if (p) free(p); }
void  operator delete(void* p, size_t) noexcept   { if (p) free(p); }
void  operator delete[](void* p, size_t) noexcept { if (p) free(p); }

// ── Symbols exported to the shared libraries ──────────────────────────────
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wreturn-type"

unsigned int aa [[gnu::used]];
unsigned int bb [[gnu::used]];
unsigned int aaa[[gnu::used]];
unsigned int bbb[[gnu::used]];

unsigned int x, y, z;
volatile int i, j = 0;
uint32_t _sgot_lib;

const char* s1       = "Help....\n";
unsigned int AA      = 5;
unsigned int BB      = 55;

extern "C" {
unsigned int f(unsigned int x) { return 3 * x; }
unsigned int w(unsigned int x) { return AA + x; }

#pragma weak mm
unsigned int mm();
unsigned int mm() { return AA + AA; }

#pragma weak lkj
unsigned int lkj();
unsigned int lkj() { return 56; }

unsigned int gfun(unsigned int x) { return x + AA + BB; }
} // extern "C"

extern unsigned int _sgot, _esgot, _sgot_plt, _esgot_plt;

// ── Global symbol table shared by all loaded libraries ────────────────────
static elf_loader::symbol_table g_global_syms;


// ── Forward declarations of test functions ────────────────────────────────
void test_bounds_validation();
void test_symbol_table(elf_loader::symbol_table& st);
void test_structured_error_strings();


// Allocate fixed buffers for the three libraries to load into RAM
alignas(8) static uint8_t ram_sh1[32768];
alignas(8) static uint8_t ram_sh2[32768];
alignas(8) static uint8_t ram_cpp[32768];
alignas(8) static uint8_t ram_tls[8192];


// ═══════════════════════════════════════════════════════════════════════════
//  Test: bounds validation
// ═══════════════════════════════════════════════════════════════════════════
void test_bounds_validation() {
    log_info("TEST: bounds validation");
    elf_loader::elf_reader efr;
    if (!efr.open_elf("libsh1/libsh1.so")) {
        log_warn("  cannot open libsh1.so for bounds test");
        return;
    }
    uint32_t needed = efr.get_binary_size();
    uint32_t small  = needed / 2;
    elf_loader::shlib_loader shl(std::move(efr));
    auto* buf = new uint8_t[small + 4];
    auto  err = shl.load_bin(buf, small);
    log_info("  load_bin(sz=%u, need=%u) → %s",
                          small, needed, elf_loader::loader_error_string(err));
    log_info("  expected InvalidSegment: %s",
                          err == elf_loader::loader_error::INVALID_SEGMENT ? "PASS" : "FAIL");
    delete[] buf;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Test: symbol_table interposition
// ═══════════════════════════════════════════════════════════════════════════
void test_symbol_table(elf_loader::symbol_table& st) {
    log_info("TEST: symbol_table interposition (first-loaded-wins)");
    const char* probes[] = {
        "aa", "bb", "library_function", "library_function2",
        "cpp_features_run", "cpp_features_summary"
    };
    for (auto name : probes) {
        uint32_t addr = st.find(name);
        log_info("  %-20s → %s  %s", name,
                              elf_loader::log::hex(addr),
                              addr ? "found" : "NOT FOUND");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Test: structured error strings
// ═══════════════════════════════════════════════════════════════════════════
void test_structured_error_strings() {
    using elf_loader::loader_error;
    log_info("TEST: structured error strings");
    log_info("  %-20s → \"%s\"", "Ok",
                           elf_loader::loader_error_string(loader_error::OK));
    log_info("  %-20s → \"%s\"", "FileNotFound",
                           elf_loader::loader_error_string(loader_error::FILE_NOT_FOUND));
    log_info("  %-20s → \"%s\"", "NotAnElf",
                           elf_loader::loader_error_string(loader_error::NOT_AN_ELF));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Test: thread_local (emulated TLS) via a loaded .so
// ═══════════════════════════════════════════════════════════════════════════
void test_tls() {
    log_info("TEST: thread_local (emulated TLS)");
    scl::Module mod_tls;
    if (!mod_tls.load_file("tls-lib/libtls_lib.so", ram_tls, sizeof ram_tls,
                           &g_global_syms)) {
        log_error("  load tls-lib/libtls_lib.so FAILED: %s",
                  mod_tls.last_error_string());
        return;
    }
    // __thread -> emulated TLS: tls_get/tls_bump resolve __emutls_v.* (GLOB_DAT)
    // and __emutls_get_address (self-contained in the .so via libgcc).
    int a = tls_get();    // 100 (initialised thread-local)
    int b = tls_bump();   // 107
    int c = tls_get();    // 107
    int z = tls_zero();   // 1
    log_info("  tls_get()=%d tls_bump()=%d tls_get()=%d tls_zero()=%d  (expect 100,107,107,1)",
             a, b, c, z);
    log_info("  %s", (a == 100 && b == 107 && c == 107 && z == 1)
                     ? "TLS PASS" : "TLS FAIL");
}

// ═══════════════════════════════════════════════════════════════════════════
//  main — entry point
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    elf_loader::log::set_verbose(true);

    if (argc < 4 || !argv || !argv[1] || !argv[2] || !argv[3]) {
        log_error("usage: %s <libsh1.so> <libsh2.so> <libcpp_features.so>",
                               argc ? argv[0] : "?");
        fflush(stdout);
        exit(1);
    }

    trace_dump_args(argc, argv);
    trace_puts("Hello Arm World! Starting direct-linking static loader tests.\n");

    // Symbols are automatically resolved from the main program's .dynsym.
    // g_global_syms registry remains empty.

    // ── Load & patch libraries using scl::Module ─────────────────────
    scl::Module mod1, mod2, mod3;

    char path1[256];
    strncpy(path1, argv[1], sizeof(path1) - 1);
    path1[sizeof(path1) - 1] = '\0';
    if (!mod1.load_file(path1, ram_sh1, sizeof(ram_sh1), &g_global_syms)) {
        snprintf(path1, sizeof(path1), "libsh1/%s", argv[1]);
        if (!mod1.load_file(path1, ram_sh1, sizeof(ram_sh1), &g_global_syms)) {
            log_error("Failed to load libsh1.so: %d", (int)mod1.last_error());
            exit(1);
        }
    }

    char path2[256];
    strncpy(path2, argv[2], sizeof(path2) - 1);
    path2[sizeof(path2) - 1] = '\0';
    if (!mod2.load_file(path2, ram_sh2, sizeof(ram_sh2), &g_global_syms)) {
        snprintf(path2, sizeof(path2), "libsh2/%s", argv[2]);
        if (!mod2.load_file(path2, ram_sh2, sizeof(ram_sh2), &g_global_syms)) {
            log_error("Failed to load libsh2.so: %d", (int)mod2.last_error());
            exit(1);
        }
    }

    char path3[256];
    strncpy(path3, argv[3], sizeof(path3) - 1);
    path3[sizeof(path3) - 1] = '\0';
    if (!mod3.load_file(path3, ram_cpp, sizeof(ram_cpp), &g_global_syms)) {
        snprintf(path3, sizeof(path3), "new-lib/%s", argv[3]);
        if (!mod3.load_file(path3, ram_cpp, sizeof(ram_cpp), &g_global_syms)) {
            log_error("Failed to load libcpp_features.so: %d", (int)mod3.last_error());
            exit(1);
        }
    }

    // Run shared diagnostics
    test_structured_error_strings();
    test_symbol_table(g_global_syms);
    test_bounds_validation();
    test_tls();

    // ── Verify Direct Calls to loaded libraries ───────────────────────
    log_info("=== VERIFY DIRECT API CALLS ===");

    // Call libsh1.so directly
    z = library_function(7);
    auto w0 = library_function2(7);
    auto q  = library_function3(7);
    *(uint32_t*)aa = 20;

    log_info("libsh1 calls: library_function(7)=%u  lib2(7)=%u  lib3(7)=%u", z, w0, q);
    log_info("aa=%lu bb=%lu aaa=%lu bbb=%lu",
                          (unsigned long)*(uint32_t*)aa,
                          (unsigned long)*(uint32_t*)bb,
                          (unsigned long)*(uint32_t*)aaa,
                          (unsigned long)*(uint32_t*)bbb);

    // Call libsh2.so directly
    log_info("boo weak = %lu", (unsigned long)boo());

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

    log_info("callback_f(7) = %u", callback_f(7));
    log_info("g(7)=%u  fff(10)=%u  gg(7)=%u", g(7), fff(10), gg(7));
    log_info("retstr=%s  retstr_=%s", retstr(), retstr_());

    char toto[] = "TOTO";
    pl2(toto);
    log_info("mm() in main = %u", mm());
    log_info("xxx() = %u", xxx());

    // Call libcpp_features.so directly
    log_info("=== C++ FEATURES DIRECT CALLS ===");
    uint32_t r0 = cpp_features_run(0, 10);
    uint32_t r1 = cpp_features_run(1, 10);
    uint32_t r2 = cpp_features_run(2, 10);
    uint32_t s  = cpp_features_summary();
    
    log_info("mode=0 (ScaleProc*3)  = %u (expect 129)",  r0);
    log_info("mode=1 (XorProc^0x55) = %u (expect 240)",  r1);
    log_info("mode=2 (ShiftProc<7>) = %u (expect 218)",  r2);
    log_info("summary() = 0x%04X (expect 0xAA55)",       s);

    // ── LED blink loop ───────────────────────────────────────────────
    timer tmr;
    tmr.start();

    for (auto& led : blink_leds) led.power_up();

    uint32_t seconds = 0;
    int loops = 5;
    if (argc > 1) loops = atoi(argv[1]);

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

    trace_puts("All direct-link static loader tests completed successfully.\n");
    fflush(stdout);
    return 0;
}

#pragma GCC diagnostic pop

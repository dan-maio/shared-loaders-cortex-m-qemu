/**
 * @file main.cpp
 * @brief Demo application — loads two shared libraries, exercises every loader feature.
 *
 * All diagnostic output uses elf_loader::log:: for consistent formatting.
 * Never use `using namespace elf_loader::log` because `log` is also a C math
 * function; always qualify with `log_info(...)`, `log::hex(...)`, etc.
 */

#include <elf-reader.h>
#include <shlib-loader.h>
#include <logger.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "diag/trace.h"
#include "timer.h"
#include "led.h"
#include "mylib1.h"
#include "mylib2.h"
#include <math.h>

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
void test_elf_reader(elf_loader::elf_reader& efr);
void test_shlib_loader(elf_loader::shlib_loader& shl);
void test_dependency_scan(elf_loader::shlib_loader& shl);
void test_patch_counts(elf_loader::shlib_loader& shl);
void test_bounds_validation();
void test_symbol_table(elf_loader::symbol_table& st);
void test_structured_error_strings();

// ═══════════════════════════════════════════════════════════════════════════
//  Helper: print GOT contents
// ═══════════════════════════════════════════════════════════════════════════
static uint32_t library_count = 0;

void print_got() {
    auto sz  = uint16_t(&_esgot - &_sgot);
    auto* p  = reinterpret_cast<uint32_t*>(&_sgot);

    log_info("GOT at %s:", elf_loader::log::hex(uintptr_t(&_sgot)));
    for (uint16_t idx = 0; idx < sz; ++idx) {
        log_info("  %03x) %s  %s", idx,
                              elf_loader::log::hex(uintptr_t(p)),
                              elf_loader::log::hex(*p));
        ++p;
    }
    auto sz2 = uint16_t(&_esgot_plt - &_sgot_plt);
    log_info("GOT.PLT at %s:", elf_loader::log::hex(uintptr_t(&_sgot_plt)));
    *p = uintptr_t(&_sgot_plt);
    for (uint16_t idx = 0; idx < sz2; ++idx) {
        log_info("  %03x) %s  %s", idx,
                              elf_loader::log::hex(uintptr_t(p)),
                              elf_loader::log::hex(*p));
        ++p;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════════════════
uint8_t* alloc_ram(uint32_t size) { return new uint8_t[size]; }
void deallocate_ram(uint8_t** buf) { delete[] *buf; *buf = nullptr; }
auto* alloc_sh_sym_patch(uint32_t n) { return new elf_loader::symbol_patch[n]; }
void deallocate_sh_sym_patch(elf_loader::symbol_patch** p) { delete[] *p; *p = nullptr; }

// ═══════════════════════════════════════════════════════════════════════════
//  Test: DT_NEEDED dependency scan
// ═══════════════════════════════════════════════════════════════════════════
void test_dependency_scan(elf_loader::shlib_loader& shl) {
    log_info("TEST: DT_NEEDED dependency scan");
    if (shl.dependencies().empty())
        log_info("  no dependencies declared");
    else
        for (auto& d : shl.dependencies())
            log_info("  NEEDED: %s", d.c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Test: patch counts (graceful failure)
// ═══════════════════════════════════════════════════════════════════════════
void test_patch_counts(elf_loader::shlib_loader& shl) {
    log_info("TEST: per-symbol patch counts (graceful failure)");
    uint32_t shlib_ok = shl.patch_shlib(&g_global_syms);
    uint32_t main_ok  = shl.patch_main(&g_global_syms);
    log_info("  shlib: %u/%u OK (%u skipped)",
         shlib_ok, shl.sh_patch_nbr, shl.sh_patch_nbr - shlib_ok);
    log_info("  main:  %u/%u OK (%u skipped)",
         main_ok, shl.main_patch_nbr, shl.main_patch_nbr - main_ok);
    log_info("  graceful failure: %s",
         (shlib_ok || !shl.sh_patch_nbr) && (main_ok || !shl.main_patch_nbr)
             ? "PASS" : "FAIL");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Test: ELF reader — dump headers, sections, .dynsym, .rel.dyn
// ═══════════════════════════════════════════════════════════════════════════
void test_elf_reader(elf_loader::elf_reader& efr) {
    efr.verify_elf();
    auto  ehdr = efr.get_header();

    log_info("binary size: %u  ehsize: %u",
                          efr.get_binary_size(), ehdr.e_ehsize);
    log_info("Section headers (%u):", ehdr.e_shnum);
    for (uint32_t i = 0; i < ehdr.e_shnum; ++i) {
        Elf32_Shdr sh;
        efr.read_section_header(i, sh);
        log_info("  %02u name=0x%04X type=0x%08X flags=0x%08X addr=%s off=%s sz=%s",
                              i, sh.sh_name, sh.sh_type, sh.sh_flags,
                              elf_loader::log::hex(sh.sh_addr),
                              elf_loader::log::hex(sh.sh_offset),
                              elf_loader::log::hex(sh.sh_size));
    }

    log_info("Program headers (%u):", ehdr.e_phnum);
    for (uint32_t i = 0; i < ehdr.e_phnum; ++i) {
        Elf32_Phdr ph;
        efr.read_program_header(i, ph);
        log_info("  %02u type=0x%04X off=%s vaddr=%s filesz=%s memsz=%s flags=0x%04X",
                              i, ph.p_type,
                              elf_loader::log::hex(ph.p_offset),
                              elf_loader::log::hex(ph.p_vaddr),
                              elf_loader::log::hex(ph.p_filesz),
                              elf_loader::log::hex(ph.p_memsz), ph.p_flags);
    }

    // Hex dump .dynsym.
    auto dn = efr.find_section_by_name(".dynsym");
    if (dn != 0xFFFFFFFF) {
        Elf32_Shdr sh;
        efr.read_section_header(dn, sh);
        auto* tab = new uint8_t[sh.sh_size];
        efr.read_section_content(dn, {tab, sh.sh_size});
        log_info(".dynsym at offset %s, %u bytes:",
                              elf_loader::log::hex(sh.sh_offset), sh.sh_size);
        for (uint32_t q = 0; q < sh.sh_size; ++q) {
            if (q % 16 == 0) printf("\n  %04lx:", (unsigned long)q);
            printf(" %02x", tab[q]);
        }
        printf("\n");
        delete[] tab;
    }

    // Hex dump .rel.dyn.
    auto rn = efr.find_section_by_name(".rel.dyn");
    if (rn != 0xFFFFFFFF) {
        Elf32_Shdr sh;
        efr.read_section_header(rn, sh);
        auto* tab = new uint8_t[sh.sh_size];
        efr.read_section_content(rn, {tab, sh.sh_size});
        log_info(".rel.dyn at offset %s, %u bytes:",
                              elf_loader::log::hex(sh.sh_offset), sh.sh_size);
        for (uint32_t q = 0; q < sh.sh_size; ++q) {
            if (q % 8 == 0) printf("\n  %04lx:", (unsigned long)q);
            printf(" %02x", tab[q]);
        }
        printf("\n");
        delete[] tab;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Test: full load/patch/constructor cycle for one library
// ═══════════════════════════════════════════════════════════════════════════
void test_shlib_loader(elf_loader::shlib_loader& shl) {
    auto& efr = shl.get_elf();
    uint32_t size = efr.get_binary_size();
    auto* buffer = alloc_ram(size + 256);

    elf_loader::loader_error err = shl.load_bin(buffer, size + 256);
    if (err != elf_loader::loader_error::OK) {
        log_error("load_bin failed: %s",
                               elf_loader::loader_error_string(err));
        exit(1);
    }

    ++library_count;
    log_info("libsh%u binary hex dump (%u bytes):",
                          unsigned(library_count), size);
    for (uint32_t i = 0; i < size; ++i) {
        if (i % 16 == 0) printf("\n  %04lx:", (unsigned long)i);
        printf(" %02x", buffer[i]);
    }
    printf("\n");

    err = shl.populate_helper_sections();
    if (err != elf_loader::loader_error::OK) {
        log_error("populate_helper_sections: %s",
                               elf_loader::loader_error_string(err));
        exit(1);
    }

    log_info("helper sections (shlib):");
    for (auto& m : shl.m_shlib)
        log_info("  %10s  sz=%s  addr=%s  es=%s",
                              m.name,
                              elf_loader::log::hex(m.size),
                              elf_loader::log::hex(uintptr_t(m.address)),
                              elf_loader::log::hex16(m.entry_size));

    log_info("helper sections (main):");
    for (auto& m : shl.m_main)
        log_info("  %10s  sz=%s  addr=%s  es=%s",
                              m.name,
                              elf_loader::log::hex(m.size),
                              elf_loader::log::hex(uintptr_t(m.address)),
                              elf_loader::log::hex16(m.entry_size));

    uint32_t ns = shl.get_shlib_patch_count();
    uint32_t nm = shl.get_main_patch_count();
    log_info("shlib patch count: %u   main patch count: %u", ns, nm);

    auto* ssp_s = alloc_sh_sym_patch(ns);
    auto* ssp_m = alloc_sh_sym_patch(nm);
    shl.allocate_patch_arrays({ssp_s, ns}, {ssp_m, nm});

    // Relocation listing.
    log_info("Shlib symbols to patch:");
    err = shl.find_shlib_symbols_to_patch({ssp_s, ns});
    if (err != elf_loader::loader_error::OK)
        log_error("find_shlib_symbols_to_patch: %s",
                               elf_loader::loader_error_string(err));
    for (uint32_t i = 0; i < shl.sh_patch_nbr; ++i)
        log_info("  %02u  got=%s  prog=%s  bind=%s  type=0x%04X  idx=%u  %.*s",
                              i,
                              elf_loader::log::hex(shl.sh_sp[i].got_address),
                              elf_loader::log::hex(shl.sh_sp[i].virtual_address),
                              elf_loader::log::binding_str(shl.sh_sp[i].binding),
                              shl.sh_sp[i].type, shl.sh_sp[i].index,
                              int(shl.sh_sp[i].name.size()), shl.sh_sp[i].name.data());

    log_info("Main symbols to patch:");
    err = shl.find_main_symbols_to_patch({ssp_m, nm});
    if (err != elf_loader::loader_error::OK)
        log_error("find_main_symbols_to_patch: %s",
                               elf_loader::loader_error_string(err));
    for (uint32_t i = 0; i < shl.main_patch_nbr; ++i)
        log_info("  %02u  got=%s  prog=%s  bind=%s  type=0x%04X  idx=%u  %.*s",
                              i,
                              elf_loader::log::hex(shl.main_sp[i].got_address),
                              elf_loader::log::hex(shl.main_sp[i].virtual_address),
                              elf_loader::log::binding_str(shl.main_sp[i].binding),
                              shl.main_sp[i].type, shl.main_sp[i].index,
                              int(shl.main_sp[i].name.size()), shl.main_sp[i].name.data());

    // Patch.
    test_dependency_scan(shl);
    test_patch_counts(shl);

    log_info("GOT after lib%u:", unsigned(library_count));
    print_got();

    // Constructors.
    shl.execute_constructors();

    // Library function calls.
    log_info("boo weak = %lu", (unsigned long)boo());

    // ret_struct() strcat()s into these fields, so they must be writable
    // buffers, not read-only string literals.
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
    log_info("lkj() in main = %lu", (unsigned long)lkj());
    log_info("lkj() in lib  = %u", shl.resolve_weak_function(lkj));

    if (library_count > 1) {
        char toto[] = "TOTO";
        pl2(toto);
        log_info("mm() in main = %u", mm());
        log_info("mm() in lib  = %u", shl.resolve_weak_function(mm));
        log_info("xxx() = %u", xxx());
    }

    deallocate_sh_sym_patch(&ssp_s);
    deallocate_sh_sym_patch(&ssp_m);
}

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
        "str", "str_", "s_lib", "retstr", "g", "ffff", "nonexistent"
    };
    for (auto name : probes) {
        uint32_t addr = st.find(name);
        log_info("  %-20s → %s  %s", name,
                              elf_loader::log::hex(addr),
                              addr ? "found" : "NOT FOUND");
    }
    uint32_t before = st.find("aa");
    st.add_global("aa", 0xDEADBEEF);
    uint32_t after = st.find("aa");
    log_info("  interposition: aa before=%s after=%s  %s",
                          elf_loader::log::hex(before),
                          elf_loader::log::hex(after),
                          before == after ? "PASS (first-wins)" : "FAIL (overwritten)");
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
    log_info("  %-20s → \"%s\"", "InvalidSegment",
                          elf_loader::loader_error_string(loader_error::INVALID_SEGMENT));
    log_info("  %-20s → \"%s\"", "InvalidReloc",
                          elf_loader::loader_error_string(loader_error::INVALID_RELOCATION));
    log_info("  %-20s → \"%s\"", "InvalidSymbol",
                          elf_loader::loader_error_string(loader_error::INVALID_SYMBOL));
    log_info("  %-20s → \"%s\"", "DepNotFound",
                          elf_loader::loader_error_string(loader_error::DEP_NOT_FOUND));
    log_info("  %-20s → \"%s\"", "OutOfMemory",
                          elf_loader::loader_error_string(loader_error::OUT_OF_MEMORY));
}

// ═══════════════════════════════════════════════════════════════════════════
//  main — entry point
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    elf_loader::log::set_verbose(true);

    if (argc < 3 || !argv || !argv[1] || !argv[2]) {
        log_error("usage: %s <libsh1.so> <libsh2.so>",
                               argc ? argv[0] : "?");
        fflush(stdout);
        exit(1);
    }

    trace_dump_args(argc, argv);
    trace_puts("Hello Arm World!\n");

    // ── Open ELF files ───────────────────────────────────────────────
    elf_loader::elf_reader efr, efr2;
    if (!efr.open_elf(argv[1]) || !efr2.open_elf(argv[2])) {
        log_error("failed to open library files");
        exit(1);
    }

    char nb1[64], nb2[64];
    auto n1 = efr.get_elf_name(), n2 = efr2.get_elf_name();
    memcpy(nb1, n1.data(), n1.size()); nb1[n1.size()] = '\0';
    memcpy(nb2, n2.data(), n2.size()); nb2[n2.size()] = '\0';
    log_info("%s  %s", nb1, nb2);

    // ── Tests ────────────────────────────────────────────────────────
    test_elf_reader(efr);
    test_elf_reader(efr2);

    elf_loader::shlib_loader shl(std::move(efr));
    log_info("shlib_loader — lib %s", nb1);
    test_shlib_loader(shl);

    elf_loader::shlib_loader shl2(std::move(efr2));
    log_info("shlib_loader — lib %s", nb2);
    test_shlib_loader(shl2);

    test_structured_error_strings();
    test_symbol_table(g_global_syms);
    test_bounds_validation();

    // ── Library calls from main ──────────────────────────────────────
    z  = library_function(7);
    auto w0 = library_function2(7);
    auto q  = library_function3(7);
    *(uint32_t*)aa = 20;

    log_info("library_function(7)=%u  lib2(7)=%u  lib3(7)=%u", z, w0, q);
    log_info("aa=%lu bb=%lu aaa=%lu bbb=%lu",
                          (unsigned long)*(uint32_t*)aa,
                          (unsigned long)*(uint32_t*)bb,
                          (unsigned long)*(uint32_t*)aaa,
                          (unsigned long)*(uint32_t*)bbb);
    log_info("g(7)=%u  fff(10)=%u  gg(7)=%u", g(7), fff(10), gg(7));
    log_info("retstr=%s  retstr_=%s", retstr(), retstr_());

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

    fflush(stdout);
    return 0;
}

#pragma GCC diagnostic pop

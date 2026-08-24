/**
 * @file main-interrupt.cpp
 * @brief relocatable loader test suite with dynamic RAM vector table patching and TIM2 interrupts.
 */
#include "board.h"
#include "semihost.h"
#include "reloc/reloc-loader.hpp"
#include "semihost-file.hpp"
#include "loader/elf-types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <cstring>
#include <cstdlib>

// Raw register access for TIM2, NVIC, SCB, and GPIOA (LED on PC13 via board.h led_set)
#define TIM2_BASE       0x40000000U
#define RCC_BASE        0x40023800U
#define NVIC_BASE       0xE000E100U
#define SCB_BASE        0xE000ED00U
#define GPIOA_BASE      0x40020000U

#define GPIOA_MODER     (*(volatile std::uint32_t*)(GPIOA_BASE + 0x00))
#define GPIOA_ODR       (*(volatile std::uint32_t*)(GPIOA_BASE + 0x14))
#define RCC_AHB1ENR     (*(volatile std::uint32_t*)(RCC_BASE + 0x30))
#define RCC_AHB1ENR_GPIOAEN (1U << 0)

inline void led_init(void) {
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA_MODER &= ~(3U << 10);
    GPIOA_MODER |= (1U << 10);
}
inline void led_on(void) {
    GPIOA_ODR |= (1U << 5);
}
inline void led_off(void) {
    GPIOA_ODR &= ~(1U << 5);
}

inline void __disable_irq(void) {
    __asm__ volatile ("cpsid i" : : : "memory");
}
inline void __enable_irq(void) {
    __asm__ volatile ("cpsie i" : : : "memory");
}

#define RCC_APB1ENR     (*(volatile uint32_t*)(RCC_BASE + 0x40))
#define RCC_CFGR        (*(volatile uint32_t*)(RCC_BASE + 0x08))
#define RCC_APB1ENR_TIM2EN (1U << 0)

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
} TIM_TypeDef;

#define TIM2 ((TIM_TypeDef*)TIM2_BASE)
#define TIM_SR_UIF   (1U << 0)
#define TIM_DIER_UIE (1U << 0)
#define TIM_CR1_CEN  (1U << 0)

#define NVIC_ISER0      (*(volatile uint32_t*)(NVIC_BASE + 0x00))
#define SCB_VTOR        (*(volatile uint32_t*)(SCB_BASE + 0x08))
#define TIM2_IRQn       28

// RAM vector table, aligned to 512 bytes (Cortex-M4 alignment requirement for 102 vectors)
__attribute__((aligned(512))) static std::uint32_t g_ram_vector_table[128];

void init_ram_vector_table(void) {
    std::uint32_t* flash_vectors = reinterpret_cast<std::uint32_t*>(SCB_VTOR);
    for (int i = 0; i < 128; i++) {
        g_ram_vector_table[i] = flash_vectors[i];
    }

    __disable_irq();
    SCB_VTOR = reinterpret_cast<std::uint32_t>(g_ram_vector_table);
    __asm__ volatile ("dsb\n isb\n");
    __enable_irq();
}

void register_tim2_isr(void (*isr)(void)) {
    __disable_irq();
    g_ram_vector_table[TIM2_IRQn + 16] = reinterpret_cast<std::uint32_t>(isr);
    __asm__ volatile ("dsb\n isb\n");
    __enable_irq();
}

// Configure TIM2 to tick at 10 kHz and overflow every 1 second (10000 ticks)
void start_tim2(void) {
    // 1. Enable TIM2 clock on APB1
    RCC_APB1ENR |= RCC_APB1ENR_TIM2EN;
    __asm__ volatile ("dsb\n isb\n");

    // 2. TIM2 clock: APB1 at 100 MHz (WeAct board, PLL from 25 MHz HSE)
    uint32_t tim_clk = 100000000;
    
    TIM2->PSC = (tim_clk / 10000) - 1;
    TIM2->ARR = 10000 - 1;

    // 3. Reset counter and enable update interrupt
    TIM2->CNT = 0;
    TIM2->SR &= ~TIM_SR_UIF; // Clear flag
    TIM2->DIER |= TIM_DIER_UIE;

    // 4. Enable TIM2 interrupt in NVIC (TIM2 is IRQ 28)
    NVIC_ISER0 = (1U << TIM2_IRQn);

    // 5. Enable TIM2 counter
    TIM2->CR1 |= TIM_CR1_CEN;
}

// Freestanding loop-based delay that does not rely on SysTick interrupts
inline void local_delay_ms(std::uint32_t ms) {
    std::uint32_t count = 50000 * ms;
    __asm__ volatile (
        "1:\n"
        "subs %0, %0, #1\n"
        "bne 1b\n"
        : "+r"(count)
        :
        : "cc"
    );
}

// Globals to store the resolved relocatable handler
extern "C" {
std::uint32_t (*g_fixture_systick_handler)(std::uint32_t) = nullptr;
}

// ---- TIM2 Interrupt Handler (fires every 1s, blinks LED for 0.1s) ----
extern "C" void my_dynamic_tim2_handler(void) {
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR &= ~TIM_SR_UIF; // Clear update interrupt flag

        // 1. Turn on LED
        led_set(1);

        // 2. Busy-wait for 100ms (0.1s) using a local loop
        local_delay_ms(100);

        // 3. Turn off LED
        led_set(0);

        // 4. Call the relocatable handler directly!
        // No PIC, GOT, or r10 base setup is required for relocatable modules.
        if (g_fixture_systick_handler) {
            g_fixture_systick_handler(1);
        }
    }
}

// ---- main's exported ABI (what the modules import) --------------------------
extern "C" {
__attribute__((used)) void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_putc('['); trace_puts(origin); trace_puts("] "); trace_puts(fn); trace_putc('(');
    char b[10]; int i = 0; std::uint32_t v = arg;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_puts(")\n");
}
__attribute__((used)) void trace_u32(const char* label, std::uint32_t v) {
    trace_puts(label);
    char b[10]; int i = 0;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_putc('\n');
}
__attribute__((used)) std::uint32_t          fm_var  = 170u;
__attribute__((used)) struct FmInfo { std::uint32_t id, flags; } fm_info = { 0xF1u, 7u };
__attribute__((used)) std::uint32_t fm_callback(std::uint32_t x) { trace_event("FM", "fm_callback", x); return x + 0x100u; }

// libm / libc the C++ fixture (cpp3) imports; provided by main (libm + crt).
float sinf(float);
float cosf(float);
float logf(float);
float sqrtf(float);

__attribute__((used)) static const void* const g_libm_anchor[] = {
    (const void*)&sinf,
    (const void*)&cosf,
    (const void*)&logf,
    (const void*)&sqrtf,
};
void* malloc(std::size_t); void free(void*);
__attribute__((used)) int* __errno(void) { static int e; return &e; }

// mylib1 & mylib2 exports
__attribute__((used)) std::uint32_t AA = 5;
__attribute__((used)) std::uint32_t BB = 55;
__attribute__((used)) const char* s1 = "Help....\n";
__attribute__((used)) std::uint32_t f(std::uint32_t x) { return 3 * x; }
__attribute__((used)) std::uint32_t w(std::uint32_t x) { return AA + x; }

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

__attribute__((used)) int printf(const char* format, ...) {
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

__attribute__((used)) char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

__attribute__((used)) double log(double x) {
    return static_cast<double>(logf(static_cast<float>(x)));
}
}





namespace {

// Stub to satisfy linker from static platform files
extern "C" void _exit(int) { while (1); }
extern "C" int  _kill(int, int) { return -1; }
extern "C" int  _getpid(void) { return 1; }

#define MAX_SYMS 512
#ifndef FIRMWARE_ELF_PATH
#define FIRMWARE_ELF_PATH "scl-fw-reloc-interrupt.elf"
#endif

struct sym_entry {
    const char* name;
    std::uint32_t addr;
};

static sym_entry g_symtab[MAX_SYMS];
static std::size_t g_syms_count = 0;

static std::size_t parse_elf_symbols(fw::semihost_file& elf) noexcept {
    trace_puts("[symtab] open OK, reading Ehdr\n");

    loader::elf::Ehdr ehdr{};
    if (!elf.read_at(0, {reinterpret_cast<std::byte*>(&ehdr), sizeof(ehdr)}))
        return 0;

    auto shoff = ehdr.e_shoff;
    auto shdr_sz = ehdr.e_shentsize;
    loader::elf::Shdr shstrtab_sh{};
    if (!elf.read_at(shoff + ehdr.e_shstrndx * shdr_sz,
            {reinterpret_cast<std::byte*>(&shstrtab_sh), shdr_sz}))
        return 0;

    loader::elf::Shdr symtab_sh{}, strtab_sh{};
    bool have_sym = false, have_str = false;
    for (unsigned i = 0; i < ehdr.e_shnum; ++i) {
        loader::elf::Shdr sh{};
        if (!elf.read_at(shoff + i * shdr_sz,
                {reinterpret_cast<std::byte*>(&sh), shdr_sz}))
            continue;
        char nm[16] = {};
        if (!elf.read_at(shstrtab_sh.sh_offset + sh.sh_name,
                {reinterpret_cast<std::byte*>(nm), sizeof(nm) - 1}))
            continue;
        if (std::strcmp(nm, ".symtab") == 0) { symtab_sh = sh; have_sym = true; }
        if (std::strcmp(nm, ".strtab") == 0) { strtab_sh = sh; have_str = true; }
    }
    if (!have_sym || !have_str) {
        trace_puts("[symtab] missing symtab or strtab\n");
        return 0;
    }

    char* strtab_buf = reinterpret_cast<char*>(malloc(strtab_sh.sh_size));
    if (!strtab_buf) {
        trace_puts("[symtab] out of memory for strtab\n");
        return 0;
    }

    if (!elf.read_at(strtab_sh.sh_offset, {reinterpret_cast<std::byte*>(strtab_buf), strtab_sh.sh_size})) {
        trace_puts("[symtab] read strtab FAILED\n");
        free(strtab_buf);
        return 0;
    }

    auto count = symtab_sh.sh_size / sizeof(loader::elf::Sym);
    trace_puts("[symtab] loading symbols...\n");
    std::size_t n = 0;

    static constexpr std::size_t BLOCK_SIZE = 64;
    static loader::elf::Sym sym_block[BLOCK_SIZE];

    std::size_t start_i = symtab_sh.sh_info;
    if (start_i >= count) start_i = 0;

    for (std::size_t i = start_i; i < count && n < MAX_SYMS; ) {
        std::size_t chunk = count - i;
        if (chunk > BLOCK_SIZE) chunk = BLOCK_SIZE;

        if (!elf.read_at(symtab_sh.sh_offset + i * sizeof(loader::elf::Sym),
                {reinterpret_cast<std::byte*>(sym_block), chunk * sizeof(loader::elf::Sym)})) {
            break;
        }

        for (std::size_t k = 0; k < chunk && n < MAX_SYMS; ++k) {
            const auto& sym = sym_block[k];
            if (sym.st_value == 0) continue;
            if (sym.st_shndx == 0 || sym.st_shndx >= 0xFFF1) continue;
            auto bind = sym.st_info >> 4;
            if (bind != 1 && bind != 2) continue;  /* STB_GLOBAL, STB_WEAK */
            
            if (sym.st_name >= strtab_sh.sh_size) continue;
            const char* name = strtab_buf + sym.st_name;
            if (!name || !*name) continue;
            g_symtab[n++] = { name, sym.st_value };
        }

        i += chunk;
    }
    trace_puts("[symtab] done parsing symbols\n");
    g_syms_count = n;
    return n;
}

static std::size_t load_symtab() noexcept {
    trace_puts("[symtab] opening ");
    trace_puts(FIRMWARE_ELF_PATH);
    trace_puts("\n");
    if (auto elf_res = fw::semihost_file::open("test-relocatable/build/" FIRMWARE_ELF_PATH)) {
        return parse_elf_symbols(*elf_res);
    }
    if (auto elf_res = fw::semihost_file::open(FIRMWARE_ELF_PATH)) {
        return parse_elf_symbols(*elf_res);
    }
    trace_puts("[symtab] open failed, trying Debug/scl-fw-reloc-interrupt.elf\n");
    if (auto elf_res = fw::semihost_file::open("Debug/scl-fw-reloc-interrupt.elf")) {
        return parse_elf_symbols(*elf_res);
    }
    trace_puts("[symtab] open failed, trying build/scl-fw-reloc-interrupt\n");
    if (auto elf_res = fw::semihost_file::open("build/scl-fw-reloc-interrupt")) {
        return parse_elf_symbols(*elf_res);
    }
    trace_puts("[symtab] open failed, trying scl-fw-reloc-interrupt\n");
    if (auto elf_res = fw::semihost_file::open("scl-fw-reloc-interrupt")) {
        return parse_elf_symbols(*elf_res);
    }
    trace_puts("[symtab] open FAILED\n");
    return 0;
}

// loader resolver thunk
loader::result<std::uint32_t> main_resolve(void* ctx, std::string_view name) noexcept {
    (void)ctx;
    for (std::size_t i = 0; i < g_syms_count; ++i) {
        if (name == g_symtab[i].name) {
            return g_symtab[i].addr;
        }
    }

    trace_puts("  [resolver] unresolved: ");
    for (char c : name) trace_putc(c);
    trace_putc('\n');
    return 0;
}

// simple semihost read_file helper
std::size_t read_file(const char* path, std::byte* dest, std::size_t cap) {
    int fd = semihost_open(path, 0); // O_RDONLY
    if (fd < 0) return 0;
    int len = semihost_flen(fd);
    if (len < 0 || static_cast<std::size_t>(len) > cap) { semihost_close(fd); return 0; }
    int r = semihost_read(fd, reinterpret_cast<char*>(dest), len);
    semihost_close(fd);
    return r == 0 ? len : 0;
}

alignas(8) std::byte g_img[3][8 * 1024];
alignas(8) std::byte g_arena[8 * 1024];
std::span<const std::byte> g_spans[3];
reloc::module_set g_set;

bool load_set_of(const char* const* names, int n) {
    if (n > 3) return false;
    for (int i = 0; i < n; ++i) {
        char path[128]; int k = 0;
        for (const char* p = RELOC_LIB_DIR "/"; *p; ++p) path[k++] = *p;
        for (const char* p = names[i]; *p; ++p) path[k++] = *p;
        for (const char* p = ".rel.o"; *p; ++p) path[k++] = *p;
        path[k] = 0;
        std::size_t sz = read_file(path, g_img[i], sizeof g_img[i]);
        if (!sz) {
            char alt_path[128]; int ak = 0;
            const char* prefix = "fixtures-reloc/build/";
            for (const char* p = prefix; *p; ++p) alt_path[ak++] = *p;
            for (const char* p = names[i]; *p; ++p) alt_path[ak++] = *p;
            for (const char* p = ".rel.o"; *p; ++p) alt_path[ak++] = *p;
            alt_path[ak] = 0;
            sz = read_file(alt_path, g_img[i], sizeof g_img[i]);
            if (!sz) {
                trace_puts("  read FAILED: "); trace_puts(path); trace_putc('\n');
                return false;
            }
        }
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
    trace_puts("\n==== scl::reloc — ET_REL module BATTERY with Interrupts ====\n");
    trace_puts("(-mlong-calls -fno-pic -r: Dynamic RAM Vector Table Patching)\n");

    // Load the symbol table automatically from the host firmware ELF
    load_symtab();

    // Initialize the RAM Vector Table
    init_ram_vector_table();
    
    // Power up the LED
    led_init();

    // ---- TEST 1: relprov + relmod (acyclic) ---------------------------------
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

    // ---- TEST 2: c1 + c2 + cpp3 (CYCLIC) ------------------------------------
    trace_puts("\n== TEST 2: c1 + c2 + cpp3 (cyclic set, load_set) ==\n");
    { const char* names[] = { "c1", "c2", "cpp3" };
      if (load_set_of(names, 3)) {
        auto c1_call_c2 = fn<u32_fn1>("c1_call_c2");
        auto c1_state   = fn<u32_fn0>("c1_state");
        auto c1_use_fm  = fn<u32_fn0>("c1_use_fm");
        auto cpp3_recurse = fn<u32_fn1>("cpp3_recurse");
        auto cpp3_method  = fn<u32_fn2>("cpp3_method");

        std::uint32_t a = c1_call_c2 ? c1_call_c2(5) : 0;
        std::uint32_t b = c1_call_c2 ? c1_call_c2(5) : 0;
        trace_u32("  c1_call_c2(5) = ", a);
        check("c1_call_c2(5) == 17467 (== ET_DYN)", a == 17467u);
        check("c1_call_c2 stateful: 2nd call differs (.bss persists)", b > a);
        check("c1_state tracked the calls", c1_state && c1_state() > 0u);

        std::uint32_t u = c1_use_fm ? c1_use_fm() : 0;
        trace_u32("  c1_use_fm() = ", u);
        check("c1_use_fm() == 611 (reads FM data)", u == 611u);

        std::uint32_t d = cpp3_recurse ? cpp3_recurse(6) : 0;
        trace_u32("  cpp3_recurse(6) = ", d);
        check("cpp3_recurse(6) == 21 (6+5+..+1)", d == 21u);

        std::uint32_t m1 = cpp3_method ? cpp3_method(2, 100) : 0;
        std::uint32_t m2 = cpp3_method ? cpp3_method(2, 100) : 0;
        trace_u32("  cpp3_method(2,100) = ", m1);
        check("cpp3_method(2,100) == 1432 (libm)", m1 == 1432u && m1 == m2);
      } else check("load_set c1+c2+cpp3", false);
    }

    // ---- TEST 3: mylib1 + mylib2 (C11 redesigned libs with Systick Handler) -
    trace_puts("\n== TEST 3: mylib1 + mylib2 (C11 modules + IRQ registration) ==\n");
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

        // Resolve relocatable my_systick_handler function pointer
        g_fixture_systick_handler = fn<u32_fn1>("my_systick_handler");
        if (g_fixture_systick_handler) {
            trace_puts("  my_systick_handler resolved successfully!\n");
        } else {
            trace_puts("  my_systick_handler resolution FAILED\n");
        }

        // Dynamically patch the RAM vector table with TIM2 Handler
        register_tim2_isr(my_dynamic_tim2_handler);
        start_tim2();

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
    
    trace_puts("\nscl-fw-reloc-interrupt: regular tests done; entering WFI loop...\n");
    semihost_exit(0);
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

namespace std {
    [[noreturn]] void __glibcxx_assert_fail(const char* file, int line, const char* function, const char* condition) {
        for (;;);
    }
}


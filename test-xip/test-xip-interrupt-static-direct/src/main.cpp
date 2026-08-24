/**
 * @file main.cpp
 * @brief XIP firmware (hosted platform) — STATIC loader flavour with Interrupts + Direct calls.
 */
#include "test-helpers.hpp"
#include "diag/trace.h"
#include "firmware/semihost-file.hpp"
#include "fs-archive.hpp"
#include "scl/image-memory.hpp"
#include "timer.h"
#include "led.h"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <span>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

#if defined(USE_HAL_DRIVER)
#include "stm32f4xx.h"
#endif

// ---- Task Scheduler Structures & Globals ----
extern "C" {
void* g_fixture_got_base = nullptr;
unsigned int (*g_fixture_systick_handler)(unsigned int) = nullptr;
volatile bool g_blink_enabled = false;
}

// Anchor the borrowed libc/libsupc++/libm set so the loaded libraries' imports
// (__dynamic_cast, printf, puts, operator new, sinf, ...) land in main's .dynsym
// and bind_host_imports can pre-link them directly.
#include "link-anchor.hpp"

/* Board LED pin: NUCLEO-F411RE = LD2 on PA5, WEACT-F411CE = PC13.
   Selected by the unified board switch (see test/platforms/board/board.h). */
#if defined(SCL_BOARD_NUCLEO)
#define BLINK_PORT_NUMBER (0)
#define BLINK_PIN_NUMBER  (5)
#else
#define BLINK_PORT_NUMBER (2)
#define BLINK_PIN_NUMBER  (13)
#endif
#define BLINK_ACTIVE_LOW  (false)
static led blink_leds[1] = {{BLINK_PORT_NUMBER, BLINK_PIN_NUMBER, BLINK_ACTIVE_LOW}};

// Precise assembly loop delay (milliseconds based on SystemCoreClock)
extern "C" void delay_ms(std::uint32_t ms) {
    std::uint32_t count = (SystemCoreClock / 3000) * ms;
    __asm__ volatile (
        "1:\n"
        "subs %0, %0, #1\n"
        "bne 1b\n"
        : "+r"(count)
        :
        : "cc"
    );
}

// ---- TIM2 Interrupt Handler (fires every 1s, blinks LED for 0.1s) ----
extern "C" void my_dynamic_tim2_handler(void) {
#if defined(USE_HAL_DRIVER)
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF; // Clear update interrupt flag

        // 1. Turn on LED
        blink_leds[0].turn_on();

        // 2. Busy-wait for 100ms (0.1s)
        delay_ms(100);

        // 3. Turn off LED
        blink_leds[0].turn_off();

        // 4. Call the library handler to test the trampoline
        if (g_fixture_systick_handler && g_fixture_got_base) {
            unsigned int toggle = 0;
            __asm__ volatile (
                "mov r4, r10\n"
                "mov r10, %1\n"
                "mov r0, #1\n"
                "blx %2\n"
                "mov %0, r0\n"
                "mov r10, r4\n"
                : "=r"(toggle)
                : "r"(g_fixture_got_base), "r"(g_fixture_systick_handler)
                : "r0", "r4", "lr", "memory"
            );
            (void)toggle;
        }
    }
#endif
}

// ---- Implement local timer methods since default timer.cpp is omitted ----
volatile timer::ticks_t timer::ms_delayCount;

void timer::start(void) {
#if defined(USE_HAL_DRIVER)
    // 1. Enable TIM2 clock on APB1
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    __asm__ volatile ("dsb\n isb\n");

    // 2. Calculate timer clock input frequency
    std::uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    std::uint32_t tim_clk = pclk1;
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0) {
        tim_clk = 2 * pclk1;
    }

    // 3. Configure Prescaler and Auto-reload for 1Hz overflow (1 second period)
    // We set TIM2 to tick at 10 kHz
    TIM2->PSC = (tim_clk / 10000) - 1;
    TIM2->ARR = 10000 - 1;

    // 4. Reset counter and enable update interrupt
    TIM2->CNT = 0;
    TIM2->SR = ~TIM_SR_UIF; // Clear interrupt flag
    TIM2->DIER |= TIM_DIER_UIE;

    // 5. Enable TIM2 interrupt in NVIC
    NVIC_EnableIRQ(TIM2_IRQn);

    // 6. Enable TIM2 counter
    TIM2->CR1 |= TIM_CR1_CEN;
#endif
    g_blink_enabled = true;
}

void timer::sleep(ticks_t ticks) {
    (void)ticks;
}

void timer::tick(void) {
}

// ---- RAM Vector Table Configuration (Dynamic Patching Method) ----
// Align to 512 bytes for Cortex-M4 vector table requirements (16 exceptions + 86 interrupts = 102 vectors)
__attribute__((aligned(512))) static std::uint32_t g_ram_vector_table[128];

void init_ram_vector_table(void) {
#if defined(USE_HAL_DRIVER)
    // 1. Copy the current vector table from Flash to RAM
    std::uint32_t* flash_vectors = reinterpret_cast<std::uint32_t*>(SCB->VTOR);
    for (int i = 0; i < 128; i++) {
        g_ram_vector_table[i] = flash_vectors[i];
    }

    // 2. Configure CPU to use the RAM Vector Table
    __disable_irq();
    SCB->VTOR = reinterpret_cast<std::uint32_t>(g_ram_vector_table);
    __DSB();
    __ISB();
    __enable_irq();
#endif
}

void register_tim2_isr(void (*isr)(void)) {
#if defined(USE_HAL_DRIVER)
    __disable_irq();
    g_ram_vector_table[TIM2_IRQn + 16] = reinterpret_cast<std::uint32_t>(isr);
    __DSB();
    __ISB();
    __enable_irq();
#endif
}

// ---- Symbols that loaded libraries import from main ----------
extern "C" {
unsigned int HOST_K = 1000;
unsigned int host_f(unsigned int x) { return x * 2 + 1; }
unsigned int AA = 42;
unsigned int BB = 84;
char*        s1 = const_cast<char*>("main-s1");
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg); }
void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v); }
std::uint32_t fm_var = 123;
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
}

// ---- Struct definitions for library globals (needed for member access) ----
extern "C" {
struct SInfo { const char* tag; unsigned int x; unsigned int y; };
extern struct SInfo s_info;

struct SLib { const char* a; float f; const char* bb; unsigned int b; unsigned int c; };
extern struct SLib s_lib;
}

namespace {

int g_bound = -99;
int g_patch1 = -99;

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
    if (src.read_at(0, std::span<std::byte>{buf, sz}).has_value()) {
        int patched = xip::stat::bind_host_imports(std::span<std::byte>{buf, sz});
        if (std::strcmp(name, "slib.so") == 0) g_bound = patched;
        if (std::strcmp(name, "mylib1.so") == 0) g_patch1 = patched;
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    return ok;
}

void test_slib() {
    trace_printf("\n[test] ==== slib.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("slib.so", m)) { trace_printf("  load FAILED\n"); return; }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");
    trace_printf("  got_base              = 0x%08x (RAM)\n", static_cast<std::uint32_t>(sm.got_base));
    trace_printf("  [bind] fn imports bound at provision = %d\n", g_bound);

    // globals + constructor effect (s_a 10->15, s_b=30 in .bss)
    trace_printf("  s_a                   = %u\n", rd(m, "s_a"));
    trace_printf("  s_b                   = %u\n", rd(m, "s_b"));
    // pure functions from flash (direct calls)
    trace_printf("  s_add(10, 5)          = %u\n", call_sym2(m, "s_add", 10, 5));
    trace_printf("  s_scale(3)            = %u\n", call_sym1(m, "s_scale", 3));
    trace_printf("  s_via_helper(5)       = %u\n", call_sym1(m, "s_via_helper", 5));
    trace_printf("  s_apply(3)            = %u\n", call_sym1(m, "s_apply", 3));
    trace_printf("  s_get_a()             = %u\n", call_sym0(m, "s_get_a"));
    // strings via R_ARM_RELATIVE
    trace_printf("  s_text()              = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "s_text")));
    trace_printf("  s_tag()               = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "s_tag")));
    // struct global s_info
    trace_printf("  s_info.tag            = \"%s\"\n", s_info.tag);
    trace_printf("  s_info.x / s_info.y   = %u / %u\n", s_info.x, s_info.y);
    // imports from main: DATA (HOST_K) + FUNCTION (host_f)
    trace_printf("  s_use_host(5)         = %u   (host_f(5)+HOST_K)\n", call_sym1(m, "s_use_host", 5));
    // FUNCTION import printf, called from flash
    trace_printf("  s_print():\n");
    call_sym0(m, "s_print");
    // mutating call LAST: s_mix(4) sets s_b using host_f
    trace_printf("  s_mix(4)              = %u\n", call_sym1(m, "s_mix", 4));
    trace_printf("  s_get_b() (after mix) = %u\n", call_sym0(m, "s_get_b"));

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

scl::Module load_and_resolve_mylib1() {
    trace_printf("\n[test] ==== load mylib1.so for SysTick ====\n");
    scl::Module m;
    if (!xload("mylib1.so", m)) { trace_printf("  load FAILED\n"); return m; }

    const auto& sm = m.get_smodule();
    void* handler = m.sym("my_systick_handler");
    g_fixture_got_base = reinterpret_cast<void*>(sm.got_base);
    g_fixture_systick_handler = reinterpret_cast<unsigned int (*)(unsigned int)>(handler);

    if (g_fixture_systick_handler) {
        trace_printf("  my_systick_handler resolved: got_base=0x%08x, handler=0x%08x\n",
                     static_cast<std::uint32_t>(sm.got_base),
                     reinterpret_cast<std::uint32_t>(g_fixture_systick_handler));
    } else {
        trace_printf("  my_systick_handler resolution FAILED\n");
    }
    return m;
}

void test_mylib1_execution(scl::Module& m) {
    trace_printf("\n[test] ==== mylib1.so execution verification ====\n");
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", rd(m, "aa"));
    trace_printf("  bb                    = %u\n", rd(m, "bb"));
    // scalar functions (pure)
    trace_printf("  library_function(39)  = %u\n", call_sym1(m, "library_function", 39));
    trace_printf("  library_function2(10) = %u\n", call_sym1(m, "library_function2", 10));
    trace_printf("  g(5)                  = %u\n", call_sym1(m, "g", 5));
    trace_printf("  gg(5)                 = %u\n", call_sym1(m, "gg", 5));
    trace_printf("  fff(5)                = %u\n", call_sym1(m, "fff", 5));
    trace_printf("  callback_f(10)        = %u\n", call_sym1(m, "callback_f", 10));
    trace_printf("  foo()                 = %u\n", call_sym0(m, "foo"));
    trace_printf("  boo()                 = %u\n", call_sym0(m, "boo"));
    trace_printf("  boo2()                = %u\n", call_sym0(m, "boo2"));
    trace_printf("  lkj()                 = %u\n", call_sym0(m, "lkj"));
    // strings via R_ARM_RELATIVE
    trace_printf("  retstr()              = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "retstr")));
    trace_printf("  retstr_()             = \"%s\"\n", reinterpret_cast<const char*>(call_sym0(m, "retstr_")));
    // struct global s_lib
    trace_printf("  s_lib.a               = \"%s\"\n", s_lib.a);
    trace_printf("  s_lib.bb              = \"%s\"\n", s_lib.bb);
    trace_printf("  s_lib.b / s_lib.c     = %u / %u\n", s_lib.b, s_lib.c);
    // void functions
    call_sym0(m, "print_lib");
    call_sym1(m, "print_lib2", 0);
    // mutating call LAST
    trace_printf("  library_function3(10) = %u\n", call_sym1(m, "library_function3", 10));
    trace_printf("  bb (after lib3)       = %u\n", rd(m, "bb"));
}

void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so (static XIP - Direct) ====\n");
    scl::Module m;
    if (!xload("mylib2.so", m)) { trace_printf("  load FAILED\n"); return; }
    const auto& sm = m.get_smodule();
    trace_printf("  loaded: %s\n", sm.ok ? "XIP" : "copy-whole");

    trace_printf("  aaa                   = %u\n", rd(m, "aaa"));
    trace_printf("  bbb                   = %u\n", rd(m, "bbb"));
    trace_printf("  xx                    = %u\n", rd(m, "xx"));
    trace_printf("  lf(39)                = %u\n", call_sym1(m, "lf", 39));
    trace_printf("  lf2(10)               = %u\n", call_sym1(m, "lf2", 10));
    trace_printf("  xxx()                 = %u\n", call_sym0(m, "xxx"));
    trace_printf("  mm()                  = %u\n", call_sym0(m, "mm"));
    trace_printf("  str2                  = \"%s\"\n", rd_str(m, "str2"));
    trace_printf("  lf3(10)               = %u\n", call_sym1(m, "lf3", 10));
    trace_printf("  bbb (after lf3)       = %u\n", rd(m, "bbb"));
    call_sym0(m, "pl");

    m = scl::Module{};
    trace_printf("  unload ok\n");
}

}  // namespace

void test_c1_c2_cpp3();
void test_ehx_cons_ppeh_ppsh_prov();
void xip_walkthrough();
void xip_walkthrough_complex();

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace_printf("\nscl-fw-xip-interrupt-static-direct: full-surface XIP test, STATIC loader + TIM2 Dynamic Interrupts + Direct calls\n");

    // Initialize the RAM vector table
    init_ram_vector_table();

    trace_printf("[prov] formatting flat-fs\n");
    if (!fw::fs_format()) trace_printf("[prov] fs_format FAILED\n");
    provision("slib.so");
    provision("mylib1.so");
    provision("mylib2.so");
    provision("c1.so");
    provision("c2.so");
    provision("cpp3.so");
    provision("libprov.so");
    provision("libcons.so");
    provision("libcppeh.so");
    provision("libcppsh.so");
    provision("ehx_catch.so");
    trace_printf("[boot] flat-fs files = %u\n", fw::fs_count());

    blink_leds[0].power_up();

    test_slib();

    // Load mylib1.so and resolve SysTick handler BEFORE starting timer
    scl::Module m1 = load_and_resolve_mylib1();

    // Dynamically patch the vector table to route TIM2 interrupts to our trampoline
    register_tim2_isr(my_dynamic_tim2_handler);

    // Start TIM2 timer interrupts (fires every 1s)
    timer t;
    t.start();

    // Execute tests
    if (m1.get_smodule().ok) {
        test_mylib1_execution(m1);
    }
    test_mylib2();
    test_c1_c2_cpp3();
    test_ehx_cons_ppeh_ppsh_prov();
    xip_walkthrough();
    xip_walkthrough_complex();

    trace_printf("\nscl-fw-xip-interrupt-static-direct: regular tests done; entering WFI loop...\n");
    std::exit(0);

    // Loop indefinitely to keep blinking the LED via dynamically patched TIM2 interrupts
    for (;;) {
        __asm__ volatile ("wfi");
    }

    // Cleanup (unreachable due to WFI loop, but kept for completeness)
    if (m1.get_smodule().ok) {
        register_tim2_isr(nullptr);
        g_fixture_systick_handler = nullptr;
        g_fixture_got_base = nullptr;
    }
}

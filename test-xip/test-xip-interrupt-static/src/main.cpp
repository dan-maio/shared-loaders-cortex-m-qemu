/**
 * @file main.cpp
 * @brief XIP firmware (hosted platform) — STATIC loader flavour with Interrupts.
 */
#include "diag/trace.h"
#include "firmware/semihost-file.hpp"
#include "fs-archive.hpp"
#include "scl/image-memory.hpp"
#include "scl/pic-trampoline.h"
#include "xip/static/adapter.hpp"
#include "timer.h"
#include "led.h"
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

#if defined(USE_HAL_DRIVER)
#include "stm32f4xx.h"
#endif

struct FmInfo { std::uint32_t id; std::uint32_t flags; };

// ---- Task Scheduler Structures & Globals ----
extern "C" {
void* g_fixture_got_base = nullptr;
unsigned int (*g_fixture_systick_handler)(unsigned int) = nullptr;
volatile bool g_blink_enabled = false;
}

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

/*
// ---- SysTick handler (kept for future reference, currently commented out) ----
#if defined(USE_HAL_DRIVER)
extern "C" void HAL_IncTick(void);
#endif

extern "C" void SysTick_Handler(void) {
#if defined(USE_HAL_DRIVER)
    HAL_IncTick();
#endif
}
*/

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

// ---- ABI the library imports from main (a static symbol registry) ----------
extern "C" {
__attribute__((used)) unsigned int HOST_K = 1000;                 // data import
__attribute__((used)) unsigned int host_f(unsigned int x) { return x * 2 + 1; }  // fn import
__attribute__((used)) unsigned int AA = 42;
__attribute__((used)) unsigned int BB = 84;
__attribute__((used)) char*        s1 = const_cast<char*>("main-s1");
__attribute__((used)) unsigned int f(unsigned int x) { return x + 10; }
__attribute__((used)) unsigned int w(unsigned int x) { return x + 20; }
__attribute__((used)) void trace_event(const char* origin, const char* fn, std::uint32_t arg) {
    trace_printf("[trace] %s: %s(%u)\n", origin, fn, arg); }
__attribute__((used)) void trace_u32(const char* label, std::uint32_t v) {
    trace_printf("[trace] %s%u\n", label, v); }
__attribute__((used)) std::uint32_t fm_var = 123;
__attribute__((used)) struct FmInfo   fm_info = {0x00F1, 0x0007};
__attribute__((used)) std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
}

#include "link-anchor.hpp"   // anchors the borrowed libc/libsupc++/libm set

namespace {

int g_bound = -99;   // #function-imports bound at provision (printed in the test)
int g_patch1 = -99;

void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    for (const char* n = name; *n && i + 1 < cap; ++n) out[i++] = *n;
    out[i] = '\0';
}
bool is_flash(std::uint32_t a) { return (a & 0xFF000000u) == 0x08000000u; }

bool provision(const char* name) {
    char path[160];
    make_path(path, sizeof(path), name);
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
    if (!ok) trace_printf("  [prov] FAILED %s\n", name);
    return ok;
}

// call/read helpers (enter the library with r10 = got_base).
std::uint32_t C0(xip::stat::SModule& m, const char* s) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), 0) : 0; }
std::uint32_t C1(xip::stat::SModule& m, const char* s, std::uint32_t a) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), a) : 0; }
std::uint32_t C2(xip::stat::SModule& m, const char* s, std::uint32_t a, std::uint32_t b) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? pic_call_u32_2(fn, reinterpret_cast<void*>(m.got_base), a, b) : 0; }
std::uint32_t RD(xip::stat::SModule& m, const char* s) {
    void* p = xip::stat::resolve(m, s); return p ? *reinterpret_cast<volatile std::uint32_t*>(p) : 0; }
const char* CSTR(xip::stat::SModule& m, const char* s) {
    void* fn = xip::stat::resolve(m, s);
    return fn ? reinterpret_cast<const char*>(pic_call_u32(fn, reinterpret_cast<void*>(m.got_base), 0)) : "(null)"; }
const char* RD_STR(xip::stat::SModule& m, const char* s) {
    void* p = xip::stat::resolve(m, s);
    return p ? *static_cast<const char**>(p) : "(null)"; }

void test_slib() {
    trace_printf("\n[test] ==== slib.so (static XIP) ====\n");
    const fw::FsEntry* e = fw::fs_find("slib.so");
    if (!e) { trace_printf("  not provisioned\n"); return; }

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m)) {
        trace_printf("  load FAILED\n"); return;
    }
    void* sadd = xip::stat::resolve(m, "s_add");
    trace_printf("  loaded: %s;  s_add @ 0x%08x (%s)\n",
                 m.ok ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(sadd),
                 is_flash(reinterpret_cast<std::uint32_t>(sadd)) ? "FLASH" : "RAM");
    trace_printf("  got_base              = 0x%08x (RAM)\n", static_cast<std::uint32_t>(m.got_base));
    trace_printf("  [bind] fn imports bound at provision = %d\n", g_bound);

    // globals + constructor effect (s_a 10->15, s_b=30 in .bss)
    trace_printf("  s_a                   = %u\n", RD(m, "s_a"));
    trace_printf("  s_b                   = %u\n", RD(m, "s_b"));
    // pure functions from flash
    trace_printf("  s_add(10, 5)          = %u\n", C2(m, "s_add", 10, 5));
    trace_printf("  s_scale(3)            = %u\n", C1(m, "s_scale", 3));
    trace_printf("  s_via_helper(5)       = %u\n", C1(m, "s_via_helper", 5));
    trace_printf("  s_apply(3)            = %u\n", C1(m, "s_apply", 3));   // fn pointer
    trace_printf("  s_get_a()             = %u\n", C0(m, "s_get_a"));
    // strings via R_ARM_RELATIVE (literal in flash .rodata; pointer relocated)
    trace_printf("  s_text()              = \"%s\"\n", CSTR(m, "s_text"));
    trace_printf("  s_tag()               = \"%s\"\n", CSTR(m, "s_tag"));
    // struct global s_info { char* tag; uint x; uint y }
    if (void* p = xip::stat::resolve(m, "s_info")) {
        auto* b = reinterpret_cast<std::uint8_t*>(p);
        trace_printf("  s_info.tag            = \"%s\"\n", *reinterpret_cast<char**>(b + 0));
        trace_printf("  s_info.x / s_info.y   = %u / %u\n",
                     *reinterpret_cast<std::uint32_t*>(b + 4), *reinterpret_cast<std::uint32_t*>(b + 8));
    }
    // imports from main: DATA (HOST_K) + FUNCTION (host_f)
    trace_printf("  s_use_host(5)         = %u   (host_f(5)+HOST_K)\n", C1(m, "s_use_host", 5));
    // FUNCTION import printf, called from flash
    trace_printf("  s_print():\n");
    C0(m, "s_print");
    // mutating call LAST: s_mix(4) sets s_b using host_f
    trace_printf("  s_mix(4)              = %u\n", C1(m, "s_mix", 4));
    trace_printf("  s_get_b() (after mix) = %u\n", C0(m, "s_get_b"));

    xip::stat::unload(m);
    trace_printf("  unload ok\n");
}

xip::stat::SModule load_and_resolve_mylib1() {
    trace_printf("\n[test] ==== load mylib1.so for SysTick ====\n");
    const fw::FsEntry* e = fw::fs_find("mylib1.so");
    xip::stat::SModule m{};
    if (!e) { trace_printf("  not provisioned\n"); return m; }

    if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m)) {
        trace_printf("  load FAILED\n"); return m;
    }
    void* handler = xip::stat::resolve(m, "my_systick_handler");
    g_fixture_got_base = reinterpret_cast<void*>(m.got_base);
    g_fixture_systick_handler = reinterpret_cast<unsigned int (*)(unsigned int)>(handler);

    if (g_fixture_systick_handler) {
        trace_printf("  my_systick_handler resolved: got_base=0x%08x, handler=0x%08x\n",
                     reinterpret_cast<std::uint32_t>(g_fixture_got_base),
                     reinterpret_cast<std::uint32_t>(g_fixture_systick_handler));
    } else {
        trace_printf("  my_systick_handler resolution FAILED\n");
    }
    return m;
}

void test_mylib1_execution(xip::stat::SModule& m) {
    trace_printf("\n[test] ==== mylib1.so execution verification ====\n");
    void* lf = xip::stat::resolve(m, "library_function");
    trace_printf("  loaded: %s;  library_function @ 0x%08x (%s)\n",
                 m.ok ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", RD(m, "aa"));
    trace_printf("  bb                    = %u\n", RD(m, "bb"));
    // scalar functions (pure — before the bb-mutating library_function3)
    trace_printf("  library_function(39)  = %u\n", C1(m, "library_function", 39));
    trace_printf("  library_function2(10) = %u\n", C1(m, "library_function2", 10));
    trace_printf("  g(5)                  = %u\n", C1(m, "g", 5));
    trace_printf("  gg(5)                 = %u\n", C1(m, "gg", 5));
    trace_printf("  fff(5)                = %u\n", C1(m, "fff", 5));
    trace_printf("  callback_f(10)        = %u\n", C1(m, "callback_f", 10));
    trace_printf("  foo()                 = %u\n", C0(m, "foo"));
    trace_printf("  boo()                 = %u\n", C0(m, "boo"));
    trace_printf("  boo2()                = %u\n", C0(m, "boo2"));
    trace_printf("  lkj()                 = %u\n", C0(m, "lkj"));
    // strings via R_ARM_RELATIVE (literal in flash .rodata; pointer relocated)
    trace_printf("  retstr()              = \"%s\"\n", CSTR(m, "retstr"));
    trace_printf("  retstr_()             = \"%s\"\n", CSTR(m, "retstr_"));
    // struct global s_lib { char*a; float f; char*bb; uint b; uint c }
    if (void* p = xip::stat::resolve(m, "s_lib")) {
        auto* b = reinterpret_cast<std::uint8_t*>(p);
        trace_printf("  s_lib.a               = \"%s\"\n", *reinterpret_cast<char**>(b + 0));
        trace_printf("  s_lib.bb              = \"%s\"\n", *reinterpret_cast<char**>(b + 8));
        trace_printf("  s_lib.b / s_lib.c     = %u / %u\n",
                     *reinterpret_cast<std::uint32_t*>(b + 12), *reinterpret_cast<std::uint32_t*>(b + 16));
    }
    // void functions (side-effects on statics; just exercise the code path)
    C0(m, "print_lib");  C1(m, "print_lib2", 0);
    // mutating call LAST: library_function3(10) sets bb, uses main's w()
    trace_printf("  library_function3(10) = %u\n", C1(m, "library_function3", 10));
    trace_printf("  bb (after lib3)       = %u\n", RD(m, "bb"));
}

void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so (static XIP) ====\n");
    const fw::FsEntry* e = fw::fs_find("mylib2.so");
    if (!e) { trace_printf("  not provisioned\n"); return; }

    xip::stat::SModule m;
    if (!xip::stat::load(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, m)) {
        trace_printf("  load FAILED\n"); return;
    }
    void* lf = xip::stat::resolve(m, "lf");
    trace_printf("  loaded: %s;  lf @ 0x%08x (%s)\n",
                 m.ok ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");

    trace_printf("  aaa                   = %u\n", RD(m, "aaa"));
    trace_printf("  bbb                   = %u\n", RD(m, "bbb"));
    trace_printf("  xx                    = %u\n", RD(m, "xx"));
    trace_printf("  lf(39)                = %u\n", C1(m, "lf", 39));
    trace_printf("  lf2(10)               = %u\n", C1(m, "lf2", 10));
    trace_printf("  xxx()                 = %u\n", C0(m, "xxx"));
    trace_printf("  mm()                  = %u\n", C0(m, "mm"));
    if (void* p = xip::stat::resolve(m, "str2"))
        trace_printf("  str2                  = \"%s\"\n", *reinterpret_cast<char**>(p));
    trace_printf("  lf3(10)               = %u\n", C1(m, "lf3", 10));
    trace_printf("  bbb (after lf3)       = %u\n", RD(m, "bbb"));
    C0(m, "pl");

    xip::stat::unload(m);
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
    trace_printf("\nscl-fw-xip-interrupt-static: full-surface XIP test, STATIC loader + TIM2 Dynamic Interrupts\n");

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
    xip::stat::SModule m1 = load_and_resolve_mylib1();

    // Dynamically patch the vector table to route TIM2 interrupts to our trampoline
    register_tim2_isr(my_dynamic_tim2_handler);

    // Start TIM2 timer interrupts (fires every 1s)
    timer t;
    t.start();

    // Execute tests
    if (m1.ok) {
        test_mylib1_execution(m1);
    }
    test_mylib2();
    test_c1_c2_cpp3();
    test_ehx_cons_ppeh_ppsh_prov();
    xip_walkthrough();
    xip_walkthrough_complex();

    trace_printf("\nscl-fw-xip-interrupt-static: regular tests done; entering WFI loop...\n");
    std::exit(0);

    // Loop indefinitely to keep blinking the LED via dynamically patched TIM2 interrupts
    for (;;) {
        __asm__ volatile ("wfi");
    }

    if (m1.ok) {
        register_tim2_isr(nullptr);
        g_fixture_systick_handler = nullptr;
        g_fixture_got_base = nullptr;
        xip::stat::unload(m1);
    }
}

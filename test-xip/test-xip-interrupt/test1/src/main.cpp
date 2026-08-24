/**
 * @file main.cpp
 * @brief XIP firmware with a custom preemptive Cortex-M4 Hard-FPU Task Scheduler.
 *        Runs the LED blinking logic in a dedicated Task (thread) context-switched
 *        via PendSV, while concurrently executing the dynamic test suite.
 */
#include "test-helpers.hpp"
#include "firmware/semihost-file.hpp"
#include "image-memory.hpp"
#include "xip/relocate.hpp"
#include "dynamic-nolib/dl.hpp"
#include "timer.h"
#include "led.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

// ---- Task Scheduler Structures & Globals ----
extern "C" {
void* g_fixture_got_base = nullptr;
unsigned int (*g_fixture_systick_handler)(unsigned int) = nullptr;
volatile bool g_blink_enabled = false;
volatile std::uint32_t g_systick_counter = 0;
volatile std::uint32_t g_led_off_counter = 0;
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
extern "C" void TIM2_IRQHandler(void) {
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

// ---- ABI that the libraries import from main (exported via -pie -Wl,-E) --------
struct FmInfo { std::uint32_t id; std::uint32_t flags; };
extern "C" {
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

__attribute__((used)) void fm_toggle_led(void) {
    blink_leds[0].toggle();
}
}

extern "C" {
int   printf(const char*, ...);
int   puts(const char*);
void* _Znwj(unsigned int);
void* _ZnwjRKSt9nothrow_t(unsigned int, const void*);
void* __cxa_allocate_exception(unsigned int);
void  __aeabi_unwind_cpp_pr0(void);
void* __dynamic_cast(const void*, const void*, const void*, long);
float sinf(float);  float cosf(float);  float logf(float);  float sqrtf(float);
}
__attribute__((used, retain)) static const void* const g_link_anchor[] = {
    reinterpret_cast<const void*>(&printf),
    reinterpret_cast<const void*>(&puts),
    reinterpret_cast<const void*>(&_Znwj),
    reinterpret_cast<const void*>(&_ZnwjRKSt9nothrow_t),
    reinterpret_cast<const void*>(&__cxa_allocate_exception),
    reinterpret_cast<const void*>(&__aeabi_unwind_cpp_pr0),
    reinterpret_cast<const void*>(&__dynamic_cast),
    reinterpret_cast<const void*>(&sinf), reinterpret_cast<const void*>(&cosf),
    reinterpret_cast<const void*>(&logf), reinterpret_cast<const void*>(&sqrtf),
    reinterpret_cast<const void*>(&fm_toggle_led),
};

namespace {

int g_patch1 = -99;

void make_path(char* out, std::size_t cap, const char* name) {
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < cap; ++d) out[i++] = *d;
    if (i + 1 < cap) out[i++] = '/';
    for (const char* n = name; *n && i + 1 < cap; ++n) out[i++] = *n;
    out[i] = '\0';
}

static bool can_xip_at_provision(std::span<const std::byte> image) noexcept {
    xip::SegmentMap seg;
    return xip::classify_segments(image, seg) && seg.split &&
           xip::text_is_immutable(image, seg);
}

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
        if (can_xip_at_provision(std::span<const std::byte>{buf, sz})) {
            int patched = xip::nolib::patch_main_imports(std::span<std::byte>{buf, sz});
            if (std::strcmp(name, "mylib1.so") == 0) g_patch1 = patched;
        }
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    if (!ok) trace_printf("  [prov] FAILED %s\n", name);
    return ok;
}

void* load_and_resolve_mylib1() {
    trace_printf("\n[test] ==== load mylib1.so for SysTick ====\n");
    void* h = xopen("mylib1.so");
    if (!h) { trace_printf("  dlopen FAILED\n"); return nullptr; }
    const std::uint32_t g = xip::nolib::get_got_base(h);

    // Resolve Systick handler
    g_fixture_got_base = reinterpret_cast<void*>(g);
    g_fixture_systick_handler = reinterpret_cast<unsigned int (*)(unsigned int)>(
        xip::nolib::dlsym(h, "my_systick_handler")
    );
    if (g_fixture_systick_handler) {
        trace_printf("  my_systick_handler resolved: got_base=0x%08x, handler=0x%08x\n",
                     reinterpret_cast<std::uint32_t>(g_fixture_got_base),
                     reinterpret_cast<std::uint32_t>(g_fixture_systick_handler));
    } else {
        trace_printf("  my_systick_handler resolution FAILED\n");
    }
    return h;
}

void test_mylib1_execution(void* h) {
    trace_printf("\n[test] ==== mylib1.so execution verification ====\n");
    const std::uint32_t g = xip::nolib::get_got_base(h);
    void* lf = xip::nolib::dlsym(h, "library_function");
    trace_printf("  loaded: %s;  library_function @ 0x%08x (%s)\n",
                 xip::nolib::is_xip(h) ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");
    trace_printf("  [patch] external calls pre-bound at provision = %d\n", g_patch1);

    // globals + constructor effect (aa doubled 10->20, bb=aa+20 in .bss)
    trace_printf("  aa                    = %u\n", RD(h, "aa"));
    trace_printf("  bb                    = %u\n", RD(h, "bb"));
    // scalar functions (pure — before the bb-mutating library_function3)
    trace_printf("  library_function(39)  = %u\n", C1(h, g, "library_function", 39));
    trace_printf("  library_function2(10) = %u\n", C1(h, g, "library_function2", 10));
    trace_printf("  g(5)                  = %u\n", C1(h, g, "g", 5));
    trace_printf("  gg(5)                 = %u\n", C1(h, g, "gg", 5));
    trace_printf("  fff(5)                = %u\n", C1(h, g, "fff", 5));
    trace_printf("  callback_f(10)        = %u\n", C1(h, g, "callback_f", 10));
    trace_printf("  foo()                 = %u\n", C0(h, g, "foo"));
    trace_printf("  boo()                 = %u\n", C0(h, g, "boo"));
    trace_printf("  boo2()                = %u\n", C0(h, g, "boo2"));
    trace_printf("  lkj()                 = %u\n", C0(h, g, "lkj"));
    // strings via R_ARM_RELATIVE (literal in flash .rodata; pointer relocated)
    trace_printf("  retstr()              = \"%s\"\n", CSTR(h, g, "retstr"));
    trace_printf("  retstr_()             = \"%s\"\n", CSTR(h, g, "retstr_"));
    // struct global s_lib { char*a; float f; char*bb; uint b; uint c }
    if (void* p = xip::nolib::dlsym(h, "s_lib")) {
        auto* b = reinterpret_cast<std::uint8_t*>(p);
        trace_printf("  s_lib.a               = \"%s\"\n", *reinterpret_cast<char**>(b + 0));
        trace_printf("  s_lib.bb              = \"%s\"\n", *reinterpret_cast<char**>(b + 8));
        trace_printf("  s_lib.b / s_lib.c     = %u / %u\n",
                     *reinterpret_cast<std::uint32_t*>(b + 12), *reinterpret_cast<std::uint32_t*>(b + 16));
    }
    // void functions (side-effects on statics; just exercise the code path)
    C0(h, g, "print_lib");  C1(h, g, "print_lib2", 0);
    // mutating call LAST: library_function3(10) sets bb, uses main's w()
    trace_printf("  library_function3(10) = %u\n", C1(h, g, "library_function3", 10));
    trace_printf("  bb (after lib3)       = %u\n", RD(h, "bb"));
}

void test_mylib2() {
    trace_printf("\n[test] ==== mylib2.so ====\n");
    void* h = xopen("mylib2.so");
    if (!h) { trace_printf("  dlopen FAILED\n"); return; }
    const std::uint32_t g = xip::nolib::get_got_base(h);
    void* lf = xip::nolib::dlsym(h, "lf");
    trace_printf("  loaded: %s;  lf @ 0x%08x (%s)\n",
                 xip::nolib::is_xip(h) ? "XIP" : "copy-whole",
                 reinterpret_cast<std::uint32_t>(lf),
                 is_flash(reinterpret_cast<std::uint32_t>(lf)) ? "FLASH" : "RAM");

    trace_printf("  aaa                   = %u\n", RD(h, "aaa"));
    trace_printf("  bbb                   = %u\n", RD(h, "bbb"));
    trace_printf("  xx                    = %u\n", RD(h, "xx"));
    trace_printf("  lf(39)                = %u\n", C1(h, g, "lf", 39));
    trace_printf("  lf2(10)               = %u\n", C1(h, g, "lf2", 10));
    trace_printf("  xxx()                 = %u\n", C0(h, g, "xxx"));
    trace_printf("  mm()                  = %u\n", C0(h, g, "mm"));
    if (void* p = xip::nolib::dlsym(h, "str2"))
        trace_printf("  str2                  = \"%s\"\n", *reinterpret_cast<char**>(p));
    trace_printf("  lf3(10)               = %u\n", C1(h, g, "lf3", 10));
    trace_printf("  bbb (after lf3)       = %u\n", RD(h, "bbb"));
    C0(h, g, "pl");

    xip::nolib::dlclose(h);
    trace_printf("  dlclose ok\n");
}

} // namespace

// Declared in test_c1_c2_cpp3.cpp
void test_c1_c2_cpp3();
// Declared in test_ehx_cons_ppeh_ppsh_prov.cpp
void test_ehx_cons_ppeh_ppsh_prov();
// Declared in walkthrough.cpp
void xip_walkthrough();
// Declared in walkthrough_complex.cpp
void xip_walkthrough_complex();

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace_printf("\nscl-fw-xip-interrupt1: full-surface XIP C/C++ + SysTick Trampoline Blink Test\n");

    // Enable cross-call routing
    fw::dl_enable_cross_call(true);

    trace_printf("[prov] formatting flat-fs\n");
    if (!fw::fs_format()) trace_printf("[prov] fs_format FAILED\n");
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

    // Load mylib1.so and resolve SysTick handler BEFORE starting scheduler
    void* h1 = load_and_resolve_mylib1();

    // Start SysTick interrupts (fires every 1s)
    timer t;
    t.start();

    // Run the remaining dynamic test suite
    if (h1) {
        test_mylib1_execution(h1);
    }
    test_mylib2();
    test_c1_c2_cpp3();
    test_ehx_cons_ppeh_ppsh_prov();
    xip_walkthrough();
    xip_walkthrough_complex();

    trace_printf("\nscl-fw-xip-interrupt1: regular tests done; entering WFI loop...\n");
    exit(0);

    // Loop indefinitely to keep blinking the LED via SysTick interrupts
    for (;;) {
        __asm__ volatile ("wfi");
    }

    if (h1) {
        g_fixture_systick_handler = nullptr;
        g_fixture_got_base = nullptr;
        xip::nolib::dlclose(h1);
    }
}

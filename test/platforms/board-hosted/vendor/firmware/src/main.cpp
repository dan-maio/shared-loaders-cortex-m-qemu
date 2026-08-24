/**
 * @file main.cpp
 * @brief Unified firmware entry point.
 * -DDYN: dynamic variant (dlopen/dlsym)
 * -DNO_DYN: non-dynamic variant (direct PLT calls)
 */
#include <stdio.h>
#include <stdlib.h>
#include "diag/trace.h"
#include "timer.h"
#include "led.h"

#if defined(DYN)
#  define DYN_VARIANT 1
#elif defined(NO_DYN)
#  define DYN_VARIANT 0
#else
#  error "Define -DDYN or -DNO_DYN"
#endif

/* ================================================================
 * Common test helpers
 * ================================================================ */
#if DYN_VARIANT
#include "firmware/dl-impl.hpp"
#include "firmware/semihost-file.hpp"
#include "loader/elf-image.hpp"
#include "dynamic-linker.hpp"

struct __attribute__((packed,aligned(4))) SL {
    char a[16]; float f; char bb[16]; unsigned b, c;
};

static void test_u1(void* h, bool& ok, const char* n, unsigned x) {
    auto fn = (unsigned(*)(unsigned))dl::dlsym((dl::handle)h, n);
    if (!fn) { trace_printf("  ? %s\n", n); ok = false; return; }
    trace_printf("  %s(%u) = %u\n", n, x, fn(x));
}
static void test_u0(void* h, bool& ok, const char* n) {
    auto fn = (unsigned(*)())dl::dlsym((dl::handle)h, n);
    if (!fn) { trace_printf("  ? %s\n", n); ok = false; return; }
    trace_printf("  %s() = %u\n", n, fn());
}
static void test_s0(void* h, bool& ok, const char* n) {
    auto fn = (const char*(*)())dl::dlsym((dl::handle)h, n);
    if (!fn) { trace_printf("  ? %s\n", n); ok = false; return; }
    trace_printf("  %s() = %s\n", n, fn());
}
static void test_v0(void* h, bool& ok, const char* n) {
    auto fn = (void(*)())dl::dlsym((dl::handle)h, n);
    if (!fn) { trace_printf("  ? %s\n", n); ok = false; return; }
    fn(); trace_printf("  %s() OK\n", n);
}
#endif

/* ================================================================
 * Dynamic variant test functions
 * ================================================================ */
#if DYN_VARIANT
#include "tests-dyn.cpp"
#endif

/* ================================================================
 * No-dyn variant: firmware stubs + direct calls
 * ================================================================ */
#if !DYN_VARIANT
extern "C" {
#include "mylib1.h"
#include "mylib2.h"
unsigned int g_cross(unsigned int);
unsigned int lf_cross(unsigned int);
}
#endif

/* Symbols exported for .so libraries — always compiled so .symtab has them.
 * Volatile pointer prevents linker from discarding as unused. */
extern "C" {
unsigned int AA = 5;
unsigned int BB = 55;
char* s1 = (char*)"Help....\n";
unsigned int f(unsigned int x) { return 3 * x; }
unsigned int w(unsigned int x) { return AA + x; }
__attribute__((weak)) unsigned int mm() { return AA + AA; }
__attribute__((weak)) unsigned int lkj() { return 56; }
#ifdef HARD_FLOAT
/* Imported by hf_test1/hf_test2, which tests-dyn.cpp dlopens in the HF suite.
 * Same definition main-no-dyn.cpp carries; it belongs to the app, not the SCL. */
float hf_pi = 3.14159265f;
#endif
float sinf(float);
float cosf(float);
float sqrtf(float);
/* Prevent discard: these symbols are only called by .so files at runtime. */
void* volatile __dl_exported[] __attribute__((used)) = {
    (void*)AA, (void*)BB, (void*)s1, (void*)f, (void*)w, (void*)mm, (void*)lkj,
    (void*)sinf, (void*)cosf, (void*)sqrtf,
};
}

/* ================================================================
 * main()
 * ================================================================ */
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    timer tim; tim.start();
    trace_puts("f411re-dynloader starting");
#if defined(SCL_BOARD_NUCLEO)
    led board_led(0, 5, false);   /* NUCLEO-F411RE LD2 on PA5 */
#else
    led board_led(2, 13, false);  /* WEACT-F411CE LED on PC13 */
#endif
    board_led.power_up();
    bool ok = true;

#if DYN_VARIANT
    test_gotplt(ok);
    test_concurrent(ok);
    test_cov(ok);
#ifndef HARD_FLOAT
    test_mylib1(ok);
    test_mylib2(ok);
    test_cross_call(ok);
    test_libcppsh(ok);
#endif
    test_c_test1(ok);
    test_c_test2(ok);
    test_cross_c1c2(ok);
    test_cross_cpp(ok);
    test_cpp_test1(ok);
    test_complex(ok);
#ifdef HARD_FLOAT
    test_hardfp(ok);
#endif
#else
    unsigned int z = library_function(42);
    trace_printf("  library_function(42) = %u\n", z);
    unsigned int gc = g_cross(7);
    trace_printf("  g_cross(7) = %u (expected 367)\n", gc);
#endif

    exit(ok ? 0 : 1);
    if (ok) { trace_puts("ALL OK — blink"); for (;;) { board_led.toggle(); timer::sleep(500); } }
    else    { trace_puts("FAIL — LED off"); board_led.turn_off(); for (;;); }
}

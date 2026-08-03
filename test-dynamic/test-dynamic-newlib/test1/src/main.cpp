/**
 * @file main.cpp
 * @brief test-static/test1 — on-target exercise of scl::static on the hosted
 *        (CMSIS/HAL/newlib) platform: load a .so through the dl:: facade,
 *        resolve and call an export, reject a bogus symbol, unload, and reload
 *        to prove the patch_journal revert left a pristine state.
 *
 * Libraries are read over semihosting from SCL_SO_DIR (an absolute path to
 * static-unify_code/libs/build, injected by CMake), so the OpenOCD CWD does
 * not matter. Output goes to the semihosting console (diag/trace).
 */
#include "diag/trace.h"
#include "timer.h"
#include "led.h"
#include "firmware/dl-impl.hpp"

#ifndef SCL_SO_DIR
#define SCL_SO_DIR "/tmp"
#endif

/* mylib1.so imports these from the firmware (resolved through the _reloc
 * .dynsym or the loader's known-symbol fallback) — same surface the
 * static-unify_code firmware provides. */
extern "C" {
unsigned int AA = 5;
unsigned int BB = 55;
char* s1 = (char*)"Help....\n";
unsigned int f(unsigned int x) { return 3 * x; }
unsigned int w(unsigned int x) { return AA + x; }
__attribute__((weak)) unsigned int mm() { return AA + AA; }
__attribute__((weak)) unsigned int lkj() { return 56; }
}

namespace {
int g_pass = 0, g_fail = 0;

void check(const char* what, bool ok) {
    trace_printf("  %s: %s\n", what, ok ? "PASS" : "FAIL");
    (ok ? g_pass : g_fail)++;
}

/** @brief One full load/call/unload cycle; returns library_function(42). */
unsigned cycle(bool& ok) {
    auto h = dl::dlopen(SCL_SO_DIR "/mylib1.so");
    if (!h) { trace_printf("  dlopen: %s\n", dl::dlerror()); ok = false; return 0; }

    auto fn = (unsigned (*)(unsigned))dl::dlsym(h, "library_function");
    if (!fn) { trace_printf("  dlsym: %s\n", dl::dlerror()); ok = false; return 0; }
    unsigned r = fn(42);

    ok = ok && (dl::dlsym(h, "no_such_symbol_xyz") == nullptr);  // negative lookup
    ok = ok && (dl::dlclose(h) == 0);
    return r;
}
} // namespace

int main(int, char**) {
    timer tim; tim.start();
    led board_led(2, 13, false);
    board_led.power_up();
    trace_puts("scl-fw-static1: static loader (scl::static) battery");

    bool ok1 = true;
    unsigned r1 = cycle(ok1);
    trace_printf("  library_function(42) = %u\n", r1);
    check("T1 load/dlsym/call/negative/dlclose", ok1 && r1 != 0);

    // T2: reload after unload — only passes if the patch_journal reverted every
    // GOT/memory patch of the first cycle (a stale patch would corrupt the
    // second resolution pass or the returned value).
    bool ok2 = true;
    unsigned r2 = cycle(ok2);
    check("T2 journal revert (reload gives identical result)", ok2 && r2 == r1);

    // T3: dlopen of a missing file fails cleanly with an error string.
    auto h = dl::dlopen(SCL_SO_DIR "/does_not_exist.so");
    check("T3 missing file rejected", h == nullptr && dl::dlerror() != nullptr);

    trace_printf("RESULT: %d passed, %d failed -> %s\n",
                 g_pass, g_fail, g_fail == 0 ? "SUCCESS" : "FAILURE");
    return g_fail == 0 ? 0 : 1;
}

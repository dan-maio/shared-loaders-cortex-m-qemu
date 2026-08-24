/**
 * @file main-no-dyn.cpp
 * @brief Firmware entry point — non-dynamic variant.
 *
 * Instead of dlopen/dlsym, this version includes library headers and
 * calls functions directly. The firmware is linked against the .so files,
 * generating PLT stubs. dynamic_linker loads the .so into RAM and patches
 * the firmware's GOT so that direct calls resolve to the loaded addresses.
 *
 * Libraries are loaded per-test-group and unloaded after to conserve RAM.
 *
 * Invocation via semihosting:
 *   f411re-no_dyn /tmp/mylib1.so /tmp/mylib2.so /tmp/g.so
 *   f411re-no_dyn /tmp/c1.so /tmp/c2.so
 *   f411re-no_dyn_hf /tmp/hf_test1.so /tmp/hf_test2.so
 */

#include <string.h>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "diag/trace.h"
#include "timer.h"
#include "led.h"
/* library headers — wrapped in extern "C" to match their C11 linkage. */
#ifndef HARD_FLOAT
extern "C" {
#include "mylib1.h"
#include "mylib2.h"
}
#else
extern "C" {
#include "mylib1.h"
#include "mylib2.h"
#include "hf-test1.h"
#include "hf-test2.h"
}
#endif
extern "C" {
#include "complex-test.h"
#include "gotplt-test.h"
#include "cov1.h"
#include "cov2.h"
}

/* For dynamic_linker + semihost_file */
#include "dynamic-linker.hpp"
#include "link-context.hpp"
#include "firmware/semihost-file.hpp"
#include "firmware/dl-load.hpp"

/* ----------------------------------------------------------------- */
/* Firmware-extern symbols expected by test libraries */
/* ----------------------------------------------------------------- */
/* AA, BB, s1, f, w, mm, lkj needed by mylib1/mylib2 for both SF and HF */
unsigned int AA = 5;
unsigned int BB = 55;
char* s1 = (char*)"Help....\n";
unsigned int f(unsigned int x) { return 3 * x; }
unsigned int w(unsigned int x) { return AA + x; }
__attribute__((weak)) unsigned int mm() { return AA + AA; }
__attribute__((weak)) unsigned int lkj() { return 56; }
extern "C" unsigned int g_cross(unsigned int);
extern "C" unsigned int lf_cross(unsigned int);
#ifdef HARD_FLOAT
float hf_pi = 3.14159265f;
#endif

/* ----------------------------------------------------------------- */
/* Linker symbols for informational printing */
/* ----------------------------------------------------------------- */
extern unsigned int _sgot;
extern unsigned int _esgot;

/** @brief Print the firmware's .got contents for debug. */
static void print_got() {
    auto size = &_esgot - &_sgot;
    auto* p = &_sgot;
    printf("GOT [%p]:\n", (void*)p);
    for (unsigned i = 0; i < size; ++i)
        printf("  [%02u] 0x%08x\n", i, p[i]);
}

/** @brief Load a library and return its handle, or NULL on failure. */
static firmware::library* load_one(firmware::dynamic_linker& linker, const char* path) {
    printf("Loading: %s\n", path);
    auto h = firmware::load_semihost(linker, path);
    if (!h) {
        printf("  FAILED to load %s\n", path);
        return nullptr;
    }
    printf("  loaded at base %p\n", (void*)(*h)->image.base());
    return *h;
}

/* ----------------------------------------------------------------- */
/* LED blink configuration (WEACT-F411CE: PA5, active high) */
/* ----------------------------------------------------------------- */
led blinkLeds[1] = { { 0, 5, false } };

/* ----------------------------------------------------------------- */
int main(int argc, char* argv[]) {
    trace_dump_args(argc, argv);

    if (argc < 2) {
        printf("Usage: main <lib1.so> [lib2.so ...]\n");
        return 1;
    }

    firmware::link_context ctx;
    firmware::dynamic_linker linker(ctx);

    /* Determine which libraries to load */
    bool has_mylib = false, has_gotplt = false, has_cov = false;
    bool has_hf = false;
    for (int i = 1; i < argc; ++i) {
        if (strstr(argv[i], "mylib")) has_mylib = true;
        if (strstr(argv[i], "gotplt") ) has_gotplt = true;
        if (strstr(argv[i], "cov1") || strstr(argv[i], "cov2") ) has_cov = true;
        if (strstr(argv[i], "hf_")) has_hf = true;
    }

    /* ============================================================ */
    /* mylib1 + mylib2 + gotplt (load once for GOT walkthrough)      */
    /* ============================================================ */
    if (has_mylib || has_gotplt) {
        for (int i = 1; i < argc; ++i) {
            if (strstr(argv[i], "mylib") || strstr(argv[i], "gotplt")) {
                if (!load_one(linker, argv[i])) return 1;
            }
        }

        print_got();

        /* --- GOT/PLT walkthrough --- */
        printf("\n========== GOT/PLT WALKTHROUGH (NO_DYN) ==========\n");
        printf("Firmware GOT at 0x%p, %u entries (patched by patch_main_got):\n",
               (void*)&_sgot, (unsigned)(&_esgot - &_sgot));
        auto* p = &_sgot;
        unsigned n = &_esgot - &_sgot;
        for (unsigned i = 0; i < n; ++i) {
            printf("  [%2u] 0x%08x", i, p[i]);
            if (i >= 3 && i < n) {
                switch (i - 3) {
                case 0: printf("  <- library_function"); break;
                case 1: printf("  <- printf (external)"); break;
                case 2: printf("  <- ret_struct"); break;
                case 3: printf("  <- g"); break;
                case 4: printf("  <- lf3"); break;
                case 5: printf("  <- library_function2"); break;
                case 6: printf("  <- xxx"); break;
                case 7: printf("  <- boo"); break;
                case 8: printf("  <- retstr"); break;
                case 9: printf("  <- lf"); break;
                case 10: printf("  <- lf2"); break;
                case 11: printf("  <- lf_cross"); break;
                case 12: printf("  <- g_cross"); break;
                default: break;
                }
            }
            printf("\n");
        }
        printf("\nEach GOT slot was patched by dynamic_linker::patch_main_got()\n");
        printf("from the loaded library's symbol table.\n");
        printf("Function calls below go through PLT -> GOT -> target.\n");
        printf("========== GOT/PLT WALKTHROUGH END ==========\n\n");

        /* --- mylib1 + mylib2 tests --- */
        if (has_mylib) {
        unsigned int z  = library_function(42);
        unsigned int w0 = library_function2(42);
        unsigned int q  = library_function3(3);
        printf("mylib1: library_function(42)=%u  library_function2(42)=%u  library_function3(3)=%u\n", z, w0, q);
        printf("mylib1: g(5)=%u  fff(7)=%u\n", g(5), fff(7));
        printf("mylib1: boo()=%u  boo2()=%u\n", boo(), boo2());
        printf("mylib1: lkj()=%u\n", lkj());
        printf("mylib1: callback_f(7)=%u\n", callback_f(7));
        printf("mylib1: retstr=\"%s\"  retstr_=\"%s\"\n", retstr(), retstr_());
        print_lib(); print_lib2((char*)"TOTO");
        printf("mylib1: gg(5)=%u\n", gg(5));
        struct Str_Lib sl1 = ret_struct_lib();
        printf("mylib1: s_lib.b=%u\n", sl1.b);
        if (argc >= 3) {
            printf("mylib2: lf(42)=%u  lf2(42)=%u  lf3(3)=%u\n", lf(42), lf2(42), lf3(3));
            printf("mylib2: xxx()=%u\n", xxx());
            pl(); pl2((char*)"TOTO2");
        }
        if (argc >= 3) {
            printf("cross: g_cross(7)=%u  lf_cross(7)=%u\n", g_cross(7), lf_cross(7));
        }
        } /* if (has_mylib) */

        /* --- gotplt_test --- */
        if (has_gotplt) {
        printf("\n--- gotplt_test.so ---\n");
        printf("  got_add(5,3)=%d  (5+3+AA)\n", got_add(5,3));
        printf("  got_compute(7)=%d  (add+mul chain)\n", got_compute(7));
        printf("  got_float_chain(0.5)=%d  (sinf+cosf+sqrtf)\n", (int)(got_float_chain(0.5f)*10000));
        {
            struct DataBlock b = got_get_block(7);
            printf("  got_get_block(7): id=%d val=%.1f label=%s  (RELATIVE ptr OK)\n", b.id, (double)b.value, b.label);
        }
        } /* if (has_gotplt) */

        /* Unload mylib + gotplt libs to free RAM */
        while (!linker.libs().empty()) linker.unload(linker.libs()[0].get());
        printf("  mylib/gotplt libs unloaded\n");
    }

    /* ============================================================ */
    /* cov1 + cov2                                                   */
    /* ============================================================ */
    if (has_cov) {
        for (int i = 1; i < argc; ++i) {
            if (strstr(argv[i], "cov1") || strstr(argv[i], "cov2")) {
                if (!load_one(linker, argv[i])) return 1;
            }
        }

        printf("\n--- COVERAGE: cov1 + cov2 ---\n");
        printf("  ABS32: cov1_global=%d (AA*2=%u)\n", cov1_global, 10u*2);
        printf("  GLOB_DAT: cov1_init_val=%d (AA+50=%u)\n", cov1_init_val(), 10u+50);
        printf("  RELATIVE: struct id=%d label=%.4s\n", cov1_get_result(5).sum, cov1_get_result(5).label);
        printf("  MATH-PLT: cov2_math_chain(0)=%d\n", (int)(cov2_math_chain(0.0f)*10000));
        printf("  CROSS cov1->cov2: cov1_cross_call(5)=%d (expected 130)\n", cov1_cross_call(5));
        printf("  CROSS cov2->cov1: cov2_call_back(7)=%d (expected 24)\n", cov2_call_back(7));
        {
            int a[] = {1, 2, 3};
            struct Cov2Result r = cov2_process(a, 3);
            printf("  ARRAY: math=%d cnt=%d tag=%s\n", (int)(r.math_val*100), r.count, r.tag);
        }

        while (!linker.libs().empty()) linker.unload(linker.libs()[0].get());
        printf("  cov libs unloaded\n");
    }

    /* ============================================================ */
    /* hf_test1 + hf_test2 (hard-float)                              */
    /* ============================================================ */
#ifdef HARD_FLOAT
    if (has_hf) {
        for (int i = 1; i < argc; ++i) {
            if (strstr(argv[i], "hf_")) {
                if (!load_one(linker, argv[i])) return 1;
            }
        }

        printf("\n--- hf_test1 + hf_test2 (hard-float) ---\n");
        printf("  hf_add(2.5,3.7)=%d\n", (int)(hf_add(2.5f,3.7f)*10));
        printf("  hf_mul(4.0,2.0)=%d\n", (int)hf_mul(4.0f,2.0f));
        printf("  hf_cross_call(3.0)=%d\n", (int)(hf_cross_call(3.0f)*10));
        printf("  hf_sincos(0.0)=%d\n", (int)(hf_sincos(0.0f)*100));
        float hf_arr[] = {5, 10, 3};
        struct FloatResult hfr = hf_compute(hf_arr, 3);
        printf("  hf_compute: sum=%d prod=%d max=%d\n", (int)(hfr.sum*10), (int)hfr.product, (int)(hfr.max*10));
        printf("  hf_test2_func(1.5)=%d\n", (int)(hf_test2_func(1.5f)*10));

        while (!linker.libs().empty()) linker.unload(linker.libs()[0].get());
        printf("  hf libs unloaded\n");
    }
#endif

    /* ============================================================ */
    /* LED blinker                                                  */
    /* ============================================================ */
    timer tmr;
    tmr.start();

    for (auto& l : blinkLeds)
        l.power_up();
    for (auto& l : blinkLeds)
        l.turn_on();
    tmr.sleep(timer::FREQUENCY_HZ);

    for (auto& l : blinkLeds)
        l.turn_off();
    tmr.sleep(timer::FREQUENCY_HZ);

    for (int i = 0; i < 5; ++i) {
        blinkLeds[0].toggle();
        tmr.sleep(timer::FREQUENCY_HZ);
        printf("Second %d\n", i + 1);
    }

    printf("Done. Halting.\n");
    exit(0);
    for (;;);
}

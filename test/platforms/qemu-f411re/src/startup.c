/**
 * @file startup.c
 * @brief STM32F411RE vector table and the PIC boot glue (QEMU variant).
 *
 * main is built plain -fpic (PC-relative GOT access), so it does NOT depend on
 * r10 -- the Reset_Handler need not set a PIC base register. It still copies the
 * contiguous init-data image (.data/.dynamic/.got/.got.plt) from its FLASH load
 * image into RAM (the GOT lives in RAM and is read PC-relatively; .dynamic must be
 * in RAM so link_context::from_self() can read _DYNAMIC), zeroes .bss, enables the
 * FPU, and calls main().
 *
 * @note Relocations are deliberately NOT applied here: a -pie image running at
 *       its link address has load bias 0, so its R_ARM_RELATIVE entries are
 *       already correct after the copy. The loader (dl.cpp) applies relocations
 *       for loaded library images (nonzero base). The loader sets r10 per-library
 *       via a trampoline only when entering single-pic-base libraries.
 *
 * QEMU difference: the emulated on-chip flash is read-only, so the flat_fs
 * "filesystem" lives in CCM SRAM (see ldscripts/mem-flatfs.ld). Real NOR flash
 * reads as 0xFF after erase; QEMU zeroes RAM, which would make every TOC slot
 * look "used". Reset_Handler therefore pre-fills the FS region with 0xFF so the
 * append-log archive sees the erased state it expects. Apps without an FS region
 * link _fs_base/_fs_size as 0 (the platform links with
 * --unresolved-symbols=ignore-all), making the fill a no-op.
 */
#include <stdint.h>
#include "semihost.h"   /* trace_puts (defined in Task 3; linked at Task 5) */

/* Linker-provided symbols (absolute addresses). */
extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;
extern uint32_t _sbss, _ebss;

int  main(void);
void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);

/** @brief Fill the RAM-backed flat_fs region with 0xFF (erased NOR state).
 *  The symbols only exist in the flat_fs build; weak references resolve to 0
 *  in every other app, making the fill a no-op (and keeping the link valid
 *  even for recipes without --unresolved-symbols=ignore-all). */
void __attribute__((noinline)) qemu_fs_erase_fill(void)
{
    extern unsigned char _fs_base[] __attribute__((weak));
    extern unsigned char _fs_size[] __attribute__((weak));
    unsigned char* p = _fs_base;
    unsigned char* e = p + (unsigned int)(uintptr_t)_fs_size;
    while (p < e) *p++ = 0xFFu;
}

/** @brief Reset entry: set GOT base, init RAM, enable FPU, call main(). */
__attribute__((naked, noreturn)) void Reset_Handler(void)
{
    __asm volatile(
        /* main is plain -fpic (PC-relative GOT); no PIC base register to set. */
        /* copy the contiguous init-data image (.data/.dynamic/.got/.got.plt) from
           its FLASH load address to RAM in one linear pass: _sidata -> _sdata.._edata. */
        "  ldr  r1, =_sidata          \n"
        "  ldr  r2, =_sdata           \n"
        "  ldr  r3, =_edata           \n"
        "1: cmp  r2, r3               \n"
        "  bcs  2f                    \n"
        "  ldr  r0, [r1], #4          \n"
        "  str  r0, [r2], #4          \n"
        "  b    1b                    \n"
        "2:                           \n"
        /* zero .bss: _sbss.._ebss */
        "  movs r0, #0                \n"
        "  ldr  r1, =_sbss            \n"
        "  ldr  r2, =_ebss            \n"
        "7: cmp  r1, r2               \n"
        "  bcs  8f                    \n"
        "  str  r0, [r1], #4          \n"
        "  b    7b                    \n"
        "8:                           \n"
        /* QEMU: pre-fill the RAM-backed flat_fs region to the erased 0xFF state */
        "  bl   qemu_fs_erase_fill    \n"
        /* enable FPU: CPACR (0xE000ED88) |= (0xF << 20) */
        "  ldr  r0, =0xE000ED88       \n"
        "  ldr  r1, [r0]              \n"
        "  orr  r1, r1, #(0xF << 20)  \n"
        "  str  r1, [r0]              \n"
        "  dsb                        \n"
        "  isb                        \n"
        /* call main(); spin if it returns */
        "  bl   main                  \n"
        "9: b    9b                   \n"
    );
}

/** @brief Catch-all exception handler: emit a marker then spin. */
void Default_Handler(void) { trace_puts("!! FAULT (default)\n"); for (;;) { } }

/** @brief Hard fault: emit a marker then spin (so a fault is visible, not silent). */
void __attribute__((weak)) HardFault_Handler(void) { trace_puts("!! HARDFAULT\n"); for (;;) { } }

/* Weak aliases so unused vectors resolve to Default_Handler. */
#define ALIAS(name) void name(void) __attribute__((weak, alias("Default_Handler")))
ALIAS(NMI_Handler);        ALIAS(MemManage_Handler);  ALIAS(BusFault_Handler);
ALIAS(UsageFault_Handler); ALIAS(SVC_Handler);        ALIAS(DebugMon_Handler);
ALIAS(PendSV_Handler);
void SysTick_Handler(void);  /* defined in board.c */

/** @brief Cortex-M4 system vector table (first 16 entries). */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[16])(void) = {
    (void (*)(void)) &_estack,  /* 0: initial SP            */
    Reset_Handler,              /* 1: reset                 */
    NMI_Handler,                /* 2                        */
    HardFault_Handler,          /* 3                        */
    MemManage_Handler,          /* 4                        */
    BusFault_Handler,           /* 5                        */
    UsageFault_Handler,         /* 6                        */
    0, 0, 0, 0,                 /* 7-10 reserved            */
    SVC_Handler,                /* 11                       */
    DebugMon_Handler,           /* 12                       */
    0,                          /* 13 reserved              */
    PendSV_Handler,             /* 14                       */
    SysTick_Handler,            /* 15                       */
};

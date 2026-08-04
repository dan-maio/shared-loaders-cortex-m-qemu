/**
 * @file board.c
 * @brief qemu-f411re board init: STM32F411RE emulated by qemu-system-arm
 *        (-machine netduinoplus2). Bare-register GPIO + SysTick.
 *
 * QEMU's STM32F405 model is a register file: the RCC never sets the
 * HSEON/PLLON "ready" bits, so the hardware board.c's `while (!(RCC_CR &
 * (1<<17)))` polls would spin forever. We therefore configure the clock from
 * the reset HSI state without polling any ready flag, and drive SysTick from a
 * nominal 84 MHz (the exact rate is irrelevant on an emulator -- tests check
 * results, not wall time).
 *
 * The LED is a real GPIOA bit (the register write is faithful to hardware) and
 * is ALSO reported over semihosting so the QEMU harness can observe and verify
 * it (QEMU does not model GPIO output pins).
 */
#include "board.h"

#define REG32(a) (*(volatile uint32_t*) (a))

/* RCC */
#define RCC_CR          REG32(0x40023800u)
#define RCC_AHB1ENR     REG32(0x40023830u)

/* GPIOA */
#define GPIOA_MODER     REG32(0x40020000u)
#define GPIOA_BSRR      REG32(0x40020018u)

/* SysTick */
#define SYST_CSR        REG32(0xE000E010u)
#define SYST_RVR        REG32(0xE000E014u)
#define SYST_CVR        REG32(0xE000E018u)

/* Nominal CPU clock used to derive the SysTick reload for a 1 ms tick. The
 * emulator core runs much faster than 84 MHz; the ratio is what matters here. */
#define QEMU_NOMINAL_CLOCK_HZ 84000000u

#define LED_PIN         5u

static volatile uint32_t s_millis = 0;

void SysTick_Handler(void)
{
    s_millis++;
}

void board_init(void)
{
    /* Enable GPIOA clock */
    RCC_AHB1ENR |= (1u << 0);

    /* PA5 = output (LD2) */
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (LED_PIN * 2)))
                | (1u << (LED_PIN * 2));

    /* No HSE/PLL switch on the emulator: stay on the reset HSI clock. */

    /* SysTick: 1 ms at the nominal clock */
    SYST_RVR = QEMU_NOMINAL_CLOCK_HZ / 1000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = (1u << 0) | (1u << 1) | (1u << 2);
}

void led_set(int on)
{
    GPIOA_BSRR = on ? (1u << LED_PIN) : (1u << (LED_PIN + 16));
    /* Observable to the QEMU harness over semihosting (QEMU has no GPIO model). */
    extern void trace_puts(const char*);
    extern void trace_putc(char);
    trace_puts("[LED:"); trace_puts(on ? "ON" : "OFF"); trace_puts("]\n");
}

void led_toggle(void)
{
    static int s_on = 0;
    s_on = !s_on;
    led_set(s_on);
}

uint32_t millis(void)
{
    return s_millis;
}

void delay_ms(uint32_t ms)
{
    uint32_t start = s_millis;
    while ((s_millis - start) < ms) { /* spin */ }
}

#include "led.h"
#include <stdint.h>

#define RCC_AHB1ENR  (*(volatile uint32_t*)(0x40023830u))
#define GPIOC_MODER  (*(volatile uint32_t*)(0x40020800u))
#define GPIOC_BSRR   (*(volatile uint32_t*)(0x40020818u))

static void delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms * 10000; i++) __asm__("nop");
}

static int initialized = 0;

void led_init(void) {
    RCC_AHB1ENR |= (1u << 2);
    __asm__ volatile("dsb" ::: "memory");
    GPIOC_MODER = (GPIOC_MODER & ~(3u << 26)) | (1u << 26);
    initialized = 1;
}

void led_blink(uint32_t count) {
    if (!initialized) return;
    for (uint32_t i = 0; i < count; i++) {
        GPIOC_BSRR = (1u << 13);
        delay_ms(200);
        GPIOC_BSRR = (1u << (13 + 16));
        delay_ms(200);
    }
}

void led_discard(void) {
    GPIOC_BSRR = (1u << (13 + 16));
    initialized = 0;
}

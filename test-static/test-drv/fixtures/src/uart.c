#include "uart.h"
#include <stdint.h>

#define RCC_APB2ENR  (*(volatile uint32_t*)(0x40023844u))
#define GPIOA_MODER  (*(volatile uint32_t*)(0x40020000u))
#define GPIOA_AFRH   (*(volatile uint32_t*)(0x40020024u))
#define GPIOA_PUPDR  (*(volatile uint32_t*)(0x4002000Cu))
#define GPIOA_OSPEEDR (*(volatile uint32_t*)(0x40020008u))
#define USART1_SR    (*(volatile uint32_t*)(0x40011000u))
#define USART1_DR    (*(volatile uint32_t*)(0x40011004u))
#define USART1_BRR   (*(volatile uint32_t*)(0x40011008u))
#define USART1_CR1   (*(volatile uint32_t*)(0x4001100Cu))

/* PCLK2 at build time: 84 MHz (NUCLEO-F411RE) or 100 MHz (WEACT-F411CE).
 * The fixture build passes -DUART_PCLK=<Hz>; default 100 MHz. */
#ifndef UART_PCLK
#define UART_PCLK 100000000u
#endif

static int initialized = 0;

void uart_init(void) {
    RCC_APB2ENR |= (1u << 4) | (1u << 0);
    __asm__ volatile("dsb" ::: "memory");

    GPIOA_MODER = (GPIOA_MODER & ~((3u << 18) | (3u << 20)))
                | ((2u << 18) | (2u << 20));
    GPIOA_AFRH = (GPIOA_AFRH & ~((0xFu << 4) | (0xFu << 8)))
               | ((7u << 4) | (7u << 8));
    GPIOA_PUPDR = (GPIOA_PUPDR & ~((3u << 18) | (3u << 20)))
                | ((1u << 18) | (1u << 20));
    GPIOA_OSPEEDR |= ((3u << 18) | (3u << 20));

    /* BRR = PCLK2 / baud (100 MHz/115200 = 868, 84 MHz/115200 = 729). */
    uint32_t brr = (UART_PCLK + 115200u / 2u) / 115200u;
    USART1_BRR = brr;
    USART1_CR1 = (1u << 13) | (1u << 3) | (1u << 2);
    initialized = 1;
}

void uart_puts(const char* s) {
    if (!initialized) return;
    while (*s) {
        while (!(USART1_SR & (1u << 7)));
        USART1_DR = (uint8_t)*s++;
    }
    /* append \r\n */
    while (!(USART1_SR & (1u << 7))); USART1_DR = '\r';
    while (!(USART1_SR & (1u << 7))); USART1_DR = '\n';
}

void uart_discard(void) {
    USART1_CR1 = 0;
    initialized = 0;
}

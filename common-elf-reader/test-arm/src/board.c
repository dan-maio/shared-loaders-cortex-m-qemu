#include "board.h"
#include <stdint.h>
#include <stdarg.h>

#define REG32(a) (*(volatile uint32_t*)(a))

/* RCC */
#define RCC_CR          REG32(0x40023800u)
#define RCC_PLLCFGR     REG32(0x40023804u)
#define RCC_CFGR        REG32(0x40023808u)
#define RCC_AHB1ENR     REG32(0x40023830u)
#define RCC_APB2ENR     REG32(0x40023844u)

/* Flash */
#define FLASH_ACR       REG32(0x40023C00u)

/* GPIOA */
#define GPIOA_MODER     REG32(0x40020000u)
#define GPIOA_OTYPER    REG32(0x40020004u)
#define GPIOA_OSPEEDR   REG32(0x40020008u)
#define GPIOA_PUPDR     REG32(0x4002000Cu)
#define GPIOA_AFRL      REG32(0x40020020u)
#define GPIOA_AFRH      REG32(0x40020024u)
#define GPIOA_BSRR      REG32(0x40020018u)

/* GPIOC */
#define GPIOC_MODER     REG32(0x40020800u)
#define GPIOC_BSRR      REG32(0x40020818u)

/* USART1 (APB2, 0x40011000) */
#define USART1_SR       REG32(0x40011000u)
#define USART1_DR       REG32(0x40011004u)
#define USART1_BRR      REG32(0x40011008u)
#define USART1_CR1      REG32(0x4001100Cu)
#define USART1_CR2      REG32(0x40011010u)
#define USART1_CR3      REG32(0x40011014u)

/* SPI1 (APB2, 0x40013000) */
#define SPI1_CR1        REG32(0x40013000u)
#define SPI1_CR2        REG32(0x40013004u)
#define SPI1_SR         REG32(0x40013008u)
#define SPI1_DR         REG32(0x4001300Cu)

/* SysTick */
#define SYST_CSR        REG32(0xE000E010u)
#define SYST_RVR        REG32(0xE000E014u)
#define SYST_CVR        REG32(0xE000E018u)

#define LED_PIN         13u
#define CS_PIN          4u

static volatile uint32_t s_millis = 0;

#if defined(USE_HAL_DRIVER)
extern void HAL_IncTick(void);
#endif

void SysTick_Handler(void)
{
#if defined(USE_HAL_DRIVER)
    HAL_IncTick();
#endif
    s_millis++;
}

void board_init(void)
{
    /* Enable GPIOA and GPIOC clocks */
    RCC_AHB1ENR |= (1u << 0) | (1u << 2);

    /* PC13 = output (LED) */
    GPIOC_MODER = (GPIOC_MODER & ~(3u << (LED_PIN * 2)))
                | (1u << (LED_PIN * 2));

    /* Flash: 3 wait states, prefetch + caches on */
    FLASH_ACR = (3u << 0) | (1u << 8) | (1u << 9) | (1u << 10);

    /* Enable HSE */
    RCC_CR |= (1u << 16);
    while (!(RCC_CR & (1u << 17)));

    /* PLL: HSE / 25 * 200 / 2 = 100 MHz */
    RCC_PLLCFGR = (25u << 0)
                | (200u << 6)
                | (0u << 16)
                | (4u << 24)
                | (1u << 22);

    /* Enable PLL */
    RCC_CR |= (1u << 24);
    while (!(RCC_CR & (1u << 25)));

    /* Configure AHB/APB: AHB=/1 (100MHz), APB1=/2 (50MHz), APB2=/1 (100MHz) */
    RCC_CFGR = (RCC_CFGR & ~((7u << 4) | (7u << 8) | (7u << 11)))
             | (0u << 4)   /* HPRE: AHB = SYSCLK / 1 */
             | (4u << 8)   /* PPRE1: APB1 = HCLK / 2 */
             | (0u << 11); /* PPRE2: APB2 = HCLK / 1 */

    /* Switch sysclk to PLL */
    RCC_CFGR = (RCC_CFGR & ~(3u << 0)) | (2u << 0);
    while ((RCC_CFGR & (3u << 2)) != (2u << 2));

    /* SysTick: 1 ms at 100 MHz */
    SYST_RVR = 100000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = (1u << 0) | (1u << 1) | (1u << 2);
}

void board_uart_init(void)
{
    /* Enable USART1 clock on APB2 */
    RCC_APB2ENR |= (1u << 4);

    /* PA9 = AF7 (TX), PA10 = AF7 (RX) */
    GPIOA_MODER = (GPIOA_MODER & ~((3u << 18) | (3u << 20)))
                | ((2u << 18) | (2u << 20));
    GPIOA_AFRH = (GPIOA_AFRH & ~((0xFu << 4) | (0xFu << 8)))
               | ((7u << 4) | (7u << 8));
    GPIOA_PUPDR = (GPIOA_PUPDR & ~((3u << 18) | (3u << 20)))
                | ((1u << 18) | (1u << 20));
    GPIOA_OSPEEDR |= ((3u << 18) | (3u << 20));

    /* 115200 baud @ 100 MHz: BRR = 54 + 4/16 = 868 */
    USART1_BRR = (54u << 4) | 4u;

    /* Enable USART, TX, RX */
    USART1_CR1 = (1u << 13) | (1u << 3) | (1u << 2);
}

void board_uart_putc(char c)
{
    while (!(USART1_SR & (1u << 7)));
    USART1_DR = (uint8_t)c;
}

void board_uart_puts(const char* s)
{
    while (*s)
    {
        if (*s == '\n') board_uart_putc('\r');
        board_uart_putc(*s++);
    }
}

void board_uart_printf(const char* fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    for (const char* p = fmt; *p; p++)
    {
        if (*p != '%') { buf[n++] = *p; if (n >= 120) { buf[n] = 0; board_uart_puts(buf); n = 0; } continue; }
        p++;
        switch (*p)
        {
        case 's':
        {
            buf[n] = 0; board_uart_puts(buf); n = 0;
            board_uart_puts(va_arg(ap, const char*));
            break;
        }
        case 'c':
            buf[n++] = (char)va_arg(ap, int);
            break;
        case 'u':
        {
            unsigned v = va_arg(ap, unsigned);
            char tmp[12]; int i = 0;
            if (v == 0) tmp[i++] = '0';
            else { while (v) { tmp[i++] = '0' + (v % 10); v /= 10; } }
            while (i) buf[n++] = tmp[--i];
            break;
        }
        case 'd':
        {
            int v = va_arg(ap, int);
            if (v < 0) { buf[n++] = '-'; v = -v; }
            unsigned uv = (unsigned)v;
            char tmp[12]; int i = 0;
            if (uv == 0) tmp[i++] = '0';
            else { while (uv) { tmp[i++] = '0' + (uv % 10); uv /= 10; } }
            while (i) buf[n++] = tmp[--i];
            break;
        }
        case 'x':
        case 'X':
        {
            unsigned v = va_arg(ap, unsigned);
            char tmp[12]; int i = 0;
            static const char hex[] = "0123456789abcdef";
            if (v == 0) tmp[i++] = '0';
            else { while (v) { tmp[i++] = hex[v % 16]; v /= 16; } }
            while (i) buf[n++] = tmp[--i];
            break;
        }
        case '%':
            buf[n++] = '%';
            break;
        default:
            buf[n++] = '%'; buf[n++] = *p;
            break;
        }
        if (n >= 120) { buf[n] = 0; board_uart_puts(buf); n = 0; }
    }
    va_end(ap);
    buf[n] = 0;
    board_uart_puts(buf);
}

void board_spi_init(void)
{
    /* Enable SPI1 clock on APB2 */
    RCC_APB2ENR |= (1u << 12);

    /* PA4 = output (CS) */
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (CS_PIN * 2)))
                | (1u << (CS_PIN * 2));
    GPIOA_BSRR = (1u << (CS_PIN + 16));

    /* PA5 = AF5 (SCK), PA6 = AF5 (MISO), PA7 = AF5 (MOSI) */
    GPIOA_MODER = (GPIOA_MODER & ~((3u << 10) | (3u << 12) | (3u << 14)))
                | ((2u << 10) | (2u << 12) | (2u << 14));
    GPIOA_AFRL = (GPIOA_AFRL & ~((0xFu << 20) | (0xFu << 24) | (0xFu << 28)))
               | ((5u << 20) | (5u << 24) | (5u << 28));
    GPIOA_OSPEEDR |= ((3u << 10) | (3u << 12) | (3u << 14));

    /* SPI1: master, /4 = 25 MHz, CPOL=0 CPHA=0, 8-bit, MSB-first */
    SPI1_CR1 = (1u << 10) | (1u << 9) | (1u << 8)
             | (1u << 2)
             | (1u << 3)
             | (1u << 6);
}

void board_spi_cs(int assert)
{
    GPIOA_BSRR = assert ? (1u << CS_PIN) : (1u << (CS_PIN + 16));
}

uint8_t board_spi_xfer(uint8_t data)
{
    while (!(SPI1_SR & (1u << 1)));
    SPI1_DR = data;
    while (!(SPI1_SR & (1u << 0)));
    return (uint8_t)SPI1_DR;
}

void board_spi_read(uint8_t* buf, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        buf[i] = board_spi_xfer(0xFF);
}

void board_spi_write(const uint8_t* buf, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        board_spi_xfer(buf[i]);
}

void led_set(int on)
{
    GPIOC_BSRR = on ? (1u << LED_PIN) : (1u << (LED_PIN + 16));
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
    while ((s_millis - start) < ms) { }
}

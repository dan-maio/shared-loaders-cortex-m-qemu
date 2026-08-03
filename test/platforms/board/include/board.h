/**
 * @file board.h
 * @brief Unified board primitives for the SCL firmware test apps.
 *
 * One header for both supported boards; the active board is selected at build
 * time with -DSCL_BOARD=nucleo|weact (set in test/cmake/scl-test-app.cmake and
 * forwarded here as SCL_BOARD_NUCLEO / SCL_BOARD_WEACT):
 *
 *   - NUCLEO-F411RE: HSE 16 MHz -> PLL 84 MHz, LED = LD2 (PA5), no UART, no SPI.
 *   - WEACT-F411CE:  HSE 25 MHz -> PLL 100 MHz, LED = PC13, USART1 + SPI1.
 *
 * Only board boot/init differs between the two; every other source in the
 * platform and all SCL/test code is shared.
 */
#ifndef FW_BOARD_H_
#define FW_BOARD_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Enable the GPIO clocks, configure the LED, start the 1 ms SysTick. */
void board_init(void);

/** @brief Turn the board LED on (@p on != 0) or off. */
void led_set(int on);

/** @brief Toggle the board LED. */
void led_toggle(void);

/** @brief Milliseconds since board_init (incremented by SysTick). */
uint32_t millis(void);

/** @brief Busy-wait @p ms milliseconds using the SysTick base. */
void delay_ms(uint32_t ms);

#if defined(SCL_BOARD_WEACT)
/* The WEACT-F411CE additionally exposes USART1 (115200 8N1 on PA9/PA10). */

/** @brief Configure USART1 (PA9 TX / PA10 RX, 115200 baud). */
void board_uart_init(void);

/** @brief Send one character on USART1 (blocking). */
void board_uart_putc(char c);

/** @brief Send a NUL-terminated string on USART1 (blocking, CRLF-aware). */
void board_uart_puts(const char* s);

/** @brief Minimal %s/%c/%u/%d/%x formatter on USART1 (no libc needed). */
void board_uart_printf(const char* fmt, ...);

/* The WEACT-F411CE additionally exposes SPI1 (PA5 SCK / PA6 MISO / PA7 MOSI,
   PA4 CS) -- the NUCLEO-F411RE has no SPI wiring, so these are absent there. */

/** @brief Configure SPI1 as master (25 MHz max, CPOL=0 CPHA=0, 8-bit, MSB). */
void board_spi_init(void);

/** @brief Assert (1) / deassert (0) the SPI1 CS line (PA4). */
void board_spi_cs(int assert);

/** @brief Transfer one byte on SPI1 (full-duplex, blocking). */
uint8_t board_spi_xfer(uint8_t data);

/** @brief Read @p len bytes from SPI1. */
void board_spi_read(uint8_t* buf, unsigned len);

/** @brief Write @p len bytes on SPI1. */
void board_spi_write(const uint8_t* buf, unsigned len);
#endif /* SCL_BOARD_WEACT */

#if defined(__cplusplus)
}
#endif

#endif /* FW_BOARD_H_ */

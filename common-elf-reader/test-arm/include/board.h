#ifndef FW_BOARD_H_
#define FW_BOARD_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

void board_init(void);

void board_uart_init(void);
void board_uart_putc(char c);
void board_uart_puts(const char* s);
void board_uart_printf(const char* fmt, ...);

void board_spi_init(void);
void board_spi_cs(int assert);
uint8_t board_spi_xfer(uint8_t data);
void board_spi_read(uint8_t* buf, unsigned len);
void board_spi_write(const uint8_t* buf, unsigned len);

void led_set(int on);
void led_toggle(void);

uint32_t millis(void);
void delay_ms(uint32_t ms);

#if defined(__cplusplus)
}
#endif

#endif

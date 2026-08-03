#ifndef UART_DRV_H
#define UART_DRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void uart_init(void);
void uart_puts(const char* s);
void uart_discard(void);

#ifdef __cplusplus
}
#endif

#endif

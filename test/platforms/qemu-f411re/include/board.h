/**
 * @file board.h
 * @brief NUCLEO-F411RE board primitives: LD2 (PA5) and a 1 ms SysTick base.
 * @note Bare-register access; HSE 16 MHz -> PLL 84 MHz.
 */
#ifndef FW_BOARD_H_
#define FW_BOARD_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief Enable GPIOA, configure PA5 as output, start the 1 ms SysTick. */
void board_init(void);

/** @brief Turn LD2 (PA5) on (@p on != 0) or off. */
void led_set(int on);

/** @brief Toggle LD2 (PA5). */
void led_toggle(void);

/** @brief Milliseconds since board_init (incremented by SysTick). */
uint32_t millis(void);

/** @brief Busy-wait @p ms milliseconds using the SysTick base. */
void delay_ms(uint32_t ms);

#if defined(__cplusplus)
}
#endif

#endif /* FW_BOARD_H_ */

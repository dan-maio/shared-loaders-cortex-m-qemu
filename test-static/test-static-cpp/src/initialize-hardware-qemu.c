/**
 * @file initialize-hardware.c (QEMU variant)
 * @brief Hardware init for the QEMU emulated STM32F411RE (netduinoplus2).
 *
 * QEMU's STM32F405 RCC never sets the HSE/PLL ready bits, so the hardware
 * clock bring-up (HSE 8 MHz -> PLL -> 100 MHz) would hang polling them. The
 * emulator needs no clock configuration: stay on the reset HSI (16 MHz) and
 * publish that value for SysTick/delay math.
 */
#include "stm32f4xx_hal.h"

extern uint32_t SystemCoreClock;

/** @brief QEMU-friendly clock config: no HSE/PLL (ready bits never set). */
void SystemClock_Config(void) {
    SystemCoreClock = 16000000u;
}

/** @brief HAL_Init + clock config — called before C++ constructors. */
void __initialize_hardware(void) {
    HAL_Init();
    SystemClock_Config();
}

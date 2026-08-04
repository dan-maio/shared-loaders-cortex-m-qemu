/**
 * @file initialize-hardware.c
 * @brief Hardware init for the QEMU emulated STM32F411RE (netduinoplus2).
 *
 * QEMU's STM32F405 RCC is a register file: the HSE/PLL "ready" bits are never
 * set, so the hardware clock bring-up (HSE 8MHz -> PLL -> 100 MHz) would hang
 * forever polling them. The emulator needs no clock configuration to run, so we
 * stay on the reset HSI clock (16 MHz) and simply publish that value. The tests
 * check loader behaviour, not wall-clock timing.
 */
#include "stm32f4xx_hal.h"

extern uint32_t SystemCoreClock;

/** @brief QEMU-friendly clock config: no HSE/PLL (the emulator never sets the
 *         ready bits); keep the reset HSI source and publish 16 MHz. */
void SystemClock_Config(void) {
    SystemCoreClock = 16000000u;
}

/** @brief HAL_Init + clock config — called before C++ constructors. */
void __initialize_hardware(void) {
    HAL_Init();
    SystemClock_Config();
}

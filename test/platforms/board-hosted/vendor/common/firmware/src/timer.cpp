/**
 * @file timer.cpp
 * @brief SysTick timer implementation for STM32F4.
 */
/*
 * Original from µOS++ (Copyright (c) 2014 Liviu Ionescu)
 * Adapted for f411re-dynloader.
 */
#include "timer.h"
#include "cortexm/exception-handlers.h"

#if defined(USE_HAL_DRIVER)
extern "C" void HAL_IncTick(void);
#endif

/** @brief Global SysTick counter, decremented by timer::tick(). */
volatile timer::ticks_t timer::ms_delayCount;

/** @brief Start the SysTick timer at FREQUENCY_HZ. */
void timer::start(void) {
    SysTick_Config(SystemCoreClock / FREQUENCY_HZ);
}

/** @brief Busy-wait for the given number of ticks. */
void timer::sleep(ticks_t ticks) {
    ms_delayCount = ticks;
    while (ms_delayCount != 0u)
        ;
}

/** @brief SysTick interrupt handler — decrements ms_delayCount. */
void timer::tick(void) {
    if (ms_delayCount != 0u)
        ms_delayCount = ms_delayCount - 1u;
}

/** @brief SysTick_Handler — calls HAL_IncTick() then timer::tick(). */
extern "C" void SysTick_Handler(void) {
#if defined(USE_HAL_DRIVER)
    HAL_IncTick();
#endif
    timer::tick();
}

/**
 * @file initialize-hardware.c
 * @brief Hosted platform hardware init (HAL + system clock).
 *
 * Unified for both supported boards; the board is selected at build time with
 * SCL_BOARD_NUCLEO / SCL_BOARD_WEACT (HSE_VALUE is set by the platform library):
 *
 *   NUCLEO-F411RE: HSE 16 MHz -> PLL 84 MHz (PLLN=336, PLLP=/4, LATENCY_2)
 *   WEACT-F411CE:  HSE 25 MHz -> PLL 100 MHz (PLLN=200, PLLP=/2, LATENCY_3)
 *
 * Only this clock configuration differs between the boards; everything else in
 * the vendored hosted platform (HAL, CMSIS, newlib glue) is shared.
 */
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_rcc.h"

#if defined(SCL_BOARD_NUCLEO)
#define PLL_N   336u            /* HSE / 16 * 336 / 4  = 84 MHz  */
#define PLL_P   RCC_PLLP_DIV4
#define LATENCY FLASH_LATENCY_2
#else                           /* SCL_BOARD_WEACT (default)      */
#define PLL_N   200u            /* HSE / 25 * 200 / 2  = 100 MHz  */
#define PLL_P   RCC_PLLP_DIV2
#define LATENCY FLASH_LATENCY_3
#endif

/** @brief Configure the HSE-driven PLL for the active board's system clock. */
void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = (HSE_VALUE/1000000u);
    RCC_OscInitStruct.PLL.PLLN       = PLL_N;
    RCC_OscInitStruct.PLL.PLLP       = PLL_P;
    RCC_OscInitStruct.PLL.PLLQ       = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) for (;;);

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, LATENCY) != HAL_OK) for (;;);
}

/** @brief HAL_Init + clock config + SystemCoreClockUpdate — called before constructors. */
void __initialize_hardware(void) {
    HAL_Init();
    SystemClock_Config();
    SystemCoreClockUpdate();
}

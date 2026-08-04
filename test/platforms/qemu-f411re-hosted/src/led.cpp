/**
 * @file led.cpp
 * @brief GPIO LED implementation for STM32F4 (QEMU variant).
 *
 * Identical GPIO behaviour to the vendored led.cpp (µOS++, MIT licence, see
 * vendor/common/firmware/src/led.cpp), plus a semihosting trace of every LED
 * state change so the QEMU harness can observe and verify the LED under the
 * emulator (QEMU models no GPIO output pins).
 */
#include "led.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"

#define BLINK_GPIOx(_N)       ((GPIO_TypeDef *)(GPIOA_BASE + (GPIOB_BASE-GPIOA_BASE)*(_N)))
#define BLINK_PIN_MASK(_N)    (1 << (_N))
#define BLINK_RCC_MASKx(_N)   (RCC_AHB1ENR_GPIOAEN << (_N))

extern "C" void trace_puts(const char* s);

namespace {

/** @brief Report a LED state over semihosting: [LED:ON] / [LED:OFF]. */
void led_report(bool on) {
    trace_puts(on ? "[LED:ON]\n" : "[LED:OFF]\n");
}

} // namespace

// ----------------------------------------------------------------------------

/** @brief Construct an LED descriptor on a GPIO port+pin. */
led::led (unsigned int port, unsigned int bit, bool activeLow)
{
  fPortNumber = port;
  fBitNumber = bit;
  fIsActiveLow = activeLow;
  fBitMask = BLINK_PIN_MASK(fBitNumber);
}

/** @brief Enable GPIO clock and configure pin as output push-pull. */
void
led::power_up ()
{
  // Enable GPIO Peripheral clock
  RCC->AHB1ENR |= BLINK_RCC_MASKx(fPortNumber);

  GPIO_InitTypeDef GPIO_InitStructure;

  // Configure pin in output push/pull mode
  GPIO_InitStructure.Pin = fBitMask;
  GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStructure.Speed = GPIO_SPEED_FAST;
  GPIO_InitStructure.Pull = GPIO_PULLUP;
  HAL_GPIO_Init (BLINK_GPIOx(fPortNumber), &GPIO_InitStructure);

  // Start with led turned off
  turn_off ();
}

/** @brief Turn the LED on (respects active-low polarity). */
void
led::turn_on ()
{
  if (fIsActiveLow)
    {
      BLINK_GPIOx(fPortNumber)->BSRR = BLINK_PIN_MASK(fBitNumber + 16);
    }
  else
    {
      BLINK_GPIOx(fPortNumber)->BSRR = fBitMask;
    }
  led_report(true);
}

void
led::turn_off ()
{
  if (fIsActiveLow)
    {
      BLINK_GPIOx(fPortNumber)->BSRR = fBitMask;
    }
  else
    {
      BLINK_GPIOx(fPortNumber)->BSRR = BLINK_PIN_MASK(fBitNumber + 16);
    }
  led_report(false);
}

void
led::toggle ()
{
  const bool now_on = isOn();
  if (now_on)
    {
      BLINK_GPIOx(fPortNumber)->ODR &= ~fBitMask;
    }
  else
    {
      BLINK_GPIOx(fPortNumber)->ODR |= fBitMask;
    }
  led_report(!now_on);
}

bool
led::isOn ()
{
  if (fIsActiveLow)
    {
      return (BLINK_GPIOx(fPortNumber)->IDR & fBitMask) == 0;
    }
  else
    {
      return (BLINK_GPIOx(fPortNumber)->IDR & fBitMask) != 0;
    }
}

// ----------------------------------------------------------------------------

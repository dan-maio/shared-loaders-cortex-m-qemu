#ifndef LED_DRV_H
#define LED_DRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void led_init(void);
void led_blink(uint32_t count);
void led_discard(void);

#ifdef __cplusplus
}
#endif

#endif

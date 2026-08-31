/** @file Minimal monotonic timer API for integration host tests. */
#ifndef __CROSS_LAYER_ESP_TIMER_H__
#define __CROSS_LAYER_ESP_TIMER_H__

#include <stdint.h>

int64_t esp_timer_get_time(void);

#endif /* __CROSS_LAYER_ESP_TIMER_H__ */

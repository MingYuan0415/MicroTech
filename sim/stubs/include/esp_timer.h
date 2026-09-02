/** @file ESP-IDF timer compatibility implemented on a host timer thread. */
#ifndef __SIM_ESP_TIMER_H__
#define __SIM_ESP_TIMER_H__

#include <stdint.h>
#include "esp_err.h"

/** @brief Opaque host timer handle. */
typedef struct host_timer *esp_timer_handle_t;

/** @brief Timer creation arguments. */
typedef struct
{
    void (*callback)(void *);
    void *arg;
    const char *dispatch_method;
    const char *name;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,
                                   uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
/** @brief Monotonic microseconds since boot. */
int64_t esp_timer_get_time(void);

#endif /* __SIM_ESP_TIMER_H__ */

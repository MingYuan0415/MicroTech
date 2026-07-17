/** @file Timer declarations for integration host tests. */
#ifndef __CROSS_LAYER_HOST_FREERTOS_H__
#define __CROSS_LAYER_HOST_FREERTOS_H__

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** @brief Opaque host timer handle. */
typedef struct host_timer *esp_timer_handle_t;

/** @brief Timer creation arguments for the host timer fake. */
typedef struct esp_timer_create_args
{
    void (*callback)(void *);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

/** @brief Create a host timer. */
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *timer);
/** @brief Start a host timer periodically. */
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,
                                   uint64_t period_us);
/** @brief Stop a host timer. */
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
/** @brief Delete a host timer. */
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
/** @brief Pause or resume a host timer. */
void host_timer_set_paused(esp_timer_handle_t timer, bool paused);
/** @brief Advance one host timer step. */
void host_timer_step(esp_timer_handle_t timer);

#endif /* __CROSS_LAYER_HOST_FREERTOS_H__ */

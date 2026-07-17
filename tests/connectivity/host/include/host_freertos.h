/** @file Fault controls and timer API for connectivity host tests. */
#ifndef __CONNECTIVITY_HOST_CONTROL_H__
#define __CONNECTIVITY_HOST_CONTROL_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** @brief Reset all host FreeRTOS fault controls. */
void host_freertos_reset_controls(void);
/** @brief Fail semaphore creation after a number of successes. */
void host_freertos_fail_semaphore_create_after(unsigned successful_creates);
/** @brief Fail the next queue creations. */
void host_freertos_fail_next_queue_creates(unsigned count);
/** @brief Fail the next task creations. */
void host_freertos_fail_next_task_creates(unsigned count);
/** @brief Fail the next queue sends. */
void host_freertos_fail_next_queue_sends(unsigned count);
/** @brief Block the next binary-semaphore give. */
void host_freertos_block_next_binary_give(void);
/** @brief Wait until the binary give fault is reached. */
bool host_freertos_wait_binary_give_blocked(uint32_t timeout_ms);
/** @brief Release a blocked binary give. */
void host_freertos_release_binary_give(void);
/** @brief Pause or resume queue receive. */
void host_freertos_pause_queue_receive(bool pause);
/** @brief Wait until queue receive pause is observed. */
bool host_freertos_wait_queue_receive_paused(uint32_t timeout_ms);
/** @brief Defer or resume queue delivery. */
void host_freertos_defer_queue_delivery(bool defer);
/** @brief Wait until queue delivery defer is observed. */
bool host_freertos_wait_queue_delivery_deferred(uint32_t timeout_ms);
/** @brief Wait for all host queues to become empty. */
bool host_freertos_wait_queues_empty(uint32_t timeout_ms);
/** @brief Advance the fake tick count. */
void host_freertos_advance_ticks(TickType_t ticks);
/** @brief Count live host semaphores. */
unsigned host_freertos_live_semaphores(void);
/** @brief Count live host queues. */
unsigned host_freertos_live_queues(void);
/** @brief Count live host tasks. */
unsigned host_freertos_live_tasks(void);
/** @brief Count successful semaphore creations. */
unsigned host_freertos_total_semaphore_creates(void);
/** @brief Count successful queue creations. */
unsigned host_freertos_total_queue_creates(void);
/** @brief Count successful task creations. */
unsigned host_freertos_total_task_creates(void);
/** @brief Wait until no host tasks remain. */
bool host_freertos_wait_no_tasks(uint32_t timeout_ms);

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

#endif /* __CONNECTIVITY_HOST_CONTROL_H__ */

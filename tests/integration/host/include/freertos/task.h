/** @file Task API declarations for integration host tests. */
#ifndef __CROSS_LAYER_FREERTOS_TASK_H__
#define __CROSS_LAYER_FREERTOS_TASK_H__

#include "freertos/FreeRTOS.h"

/** @brief Notification update actions modeled by the host fixture. */
typedef enum
{
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
} eNotifyAction;

/** @brief Return the current host task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/** @brief Create a pthread-backed static task. */
TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *context,
                               UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *task_storage);
/** @brief Send a notification to a host task. */
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action);
/** @brief Wait for a host task notification. */
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout_ticks);

#endif /* __CROSS_LAYER_FREERTOS_TASK_H__ */

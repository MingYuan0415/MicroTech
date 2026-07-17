/** @file Task API declarations for the connectivity host fixture. */
#ifndef __CONNECTIVITY_HOST_FREERTOS_TASK_H__
#define __CONNECTIVITY_HOST_FREERTOS_TASK_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a pthread-backed static task. */
TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *context,
                               UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *task_storage);
/** @brief Return the current host task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/** @brief Return the host tick count. */
TickType_t xTaskGetTickCount(void);
/** @brief Delay the current host task. */
void vTaskDelay(TickType_t ticks);
/** @brief Delete a host task. */
void vTaskDelete(TaskHandle_t task);

#endif /* __CONNECTIVITY_HOST_FREERTOS_TASK_H__ */

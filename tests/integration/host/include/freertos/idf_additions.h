/** @file ESP-IDF FreeRTOS additions modeled by integration host tests. */
#ifndef __CROSS_LAYER_FREERTOS_IDF_ADDITIONS_H__
#define __CROSS_LAYER_FREERTOS_IDF_ADDITIONS_H__

#include "freertos/task.h"

/** @brief Create a pthread-backed task with recorded stack capabilities. */
BaseType_t xTaskCreateWithCaps(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *context,
                               UBaseType_t priority, TaskHandle_t *out_task,
                               UBaseType_t memory_caps);
/** @brief Delete a task created by xTaskCreateWithCaps(). */
void vTaskDeleteWithCaps(TaskHandle_t task);

#endif /* __CROSS_LAYER_FREERTOS_IDF_ADDITIONS_H__ */

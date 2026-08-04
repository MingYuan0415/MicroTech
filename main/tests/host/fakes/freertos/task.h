#ifndef __FREERTOS_TASK_H__
#define __FREERTOS_TASK_H__

#include "freertos/FreeRTOS.h"

/**
 * @brief Yield the host test thread for a scripted tick count.
 *
 * @param ticks_to_delay is the requested delay in fake ticks.
 */
void vTaskDelay(TickType_t ticks_to_delay);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks);
void vTaskSuspend(TaskHandle_t task);
TaskHandle_t xTaskGetHandle(const char *name);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
StackType_t *xTaskGetStackStart(TaskHandle_t task);

#endif /* __FREERTOS_TASK_H__ */

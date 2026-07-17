#ifndef __FREERTOS_TASK_H__
#define __FREERTOS_TASK_H__

#include "freertos/FreeRTOS.h"

/**
 * @brief Yield the host test thread for a scripted tick count.
 *
 * @param ticks_to_delay is the requested delay in fake ticks.
 */
void vTaskDelay(TickType_t ticks_to_delay);

#endif /* __FREERTOS_TASK_H__ */

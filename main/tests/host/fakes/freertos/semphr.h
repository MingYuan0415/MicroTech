#ifndef __MAIN_HOST_FREERTOS_SEMPHR_H__
#define __MAIN_HOST_FREERTOS_SEMPHR_H__

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#endif /* __MAIN_HOST_FREERTOS_SEMPHR_H__ */

/** @file Semaphore API declarations for integration host tests. */
#ifndef __CROSS_LAYER_FREERTOS_SEMPHR_H__
#define __CROSS_LAYER_FREERTOS_SEMPHR_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a host binary semaphore. */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
/** @brief Create a host mutex. */
SemaphoreHandle_t xSemaphoreCreateMutex(void);
/** @brief Take a host semaphore. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
/** @brief Give a host semaphore. */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
/** @brief Delete a host semaphore. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#endif /* __CROSS_LAYER_FREERTOS_SEMPHR_H__ */

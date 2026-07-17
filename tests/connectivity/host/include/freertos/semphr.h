/** @file Semaphore API declarations for the connectivity host fixture. */
#ifndef __CONNECTIVITY_HOST_FREERTOS_SEMPHR_H__
#define __CONNECTIVITY_HOST_FREERTOS_SEMPHR_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a host mutex. */
SemaphoreHandle_t xSemaphoreCreateMutex(void);
/** @brief Create a host binary semaphore. */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
/** @brief Create a mutex in caller-provided storage. */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
/** @brief Create a binary semaphore in caller-provided storage. */
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
/** @brief Take a host semaphore. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t timeout_ticks);
/** @brief Give a host semaphore. */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
/** @brief Delete a host semaphore. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#endif /* __CONNECTIVITY_HOST_FREERTOS_SEMPHR_H__ */

/** @file Semaphore API declarations for the MicroTech simulator host port. */
#ifndef __SIM_FREERTOS_SEMPHR_H__
#define __SIM_FREERTOS_SEMPHR_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a host binary semaphore. */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
/** @brief Create a binary semaphore inside caller-provided storage. */
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
/** @brief Create a host mutex. */
SemaphoreHandle_t xSemaphoreCreateMutex(void);
/** @brief Create a mutex inside caller-provided storage. */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
/** @brief Create a host recursive mutex. */
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
/** @brief Create a recursive mutex inside caller-provided storage. */
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(
    StaticSemaphore_t *storage);
/** @brief Take a host semaphore. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
/** @brief Take a host recursive mutex. */
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore,
                                   TickType_t timeout);
/** @brief Give a host semaphore. */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
/** @brief Give a host recursive mutex. */
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore);
/** @brief Delete a host semaphore. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#endif /* __SIM_FREERTOS_SEMPHR_H__ */

#ifndef __FREERTOS_FREERTOS_H__
#define __FREERTOS_FREERTOS_H__

#include <stdint.h>

/** @brief Host representation of a FreeRTOS tick count. */
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdFALSE       0
#define pdTRUE        1
#define pdFAIL        0
#define pdPASS        1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))

#endif /* __FREERTOS_FREERTOS_H__ */

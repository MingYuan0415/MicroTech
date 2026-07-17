/** @file Pthread-backed FreeRTOS types used by integration tests. */
#ifndef __CROSS_LAYER_FREERTOS_H__
#define __CROSS_LAYER_FREERTOS_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;

/** @brief Pthread-backed static task storage. */
typedef struct host_static_task
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t notification_ready;
    uint32_t notification;
    void (*entry)(void *);
    void *context;
    bool created;
    bool shutdown;
} StaticTask_t;

typedef StaticTask_t *TaskHandle_t;
/** @brief Opaque host queue handle. */
typedef struct host_queue *QueueHandle_t;
/** @brief Opaque host semaphore handle. */
typedef struct host_semaphore *SemaphoreHandle_t;
typedef pthread_mutex_t portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#ifndef configTICK_RATE_HZ
    #define configTICK_RATE_HZ 1000U
#endif /* __CROSS_LAYER_FREERTOS_H__ */

#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(lock) ((void)pthread_mutex_lock((lock)))
#define taskEXIT_CRITICAL(lock) ((void)pthread_mutex_unlock((lock)))

#endif

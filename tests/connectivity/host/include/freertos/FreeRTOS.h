/** @file Pthread-backed FreeRTOS types used by connectivity tests. */
#ifndef __CONNECTIVITY_HOST_FREERTOS_H__
#define __CONNECTIVITY_HOST_FREERTOS_H__

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;

/** @brief Pthread-backed static queue storage. */
typedef struct host_static_queue
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint8_t *items;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool initialized;
} StaticQueue_t;

/** @brief Pthread-backed static semaphore storage. */
typedef struct host_static_semaphore
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool available;
    bool initialized;
    bool is_binary;
    bool recursive;
    pthread_t owner;
    unsigned int depth;
} StaticSemaphore_t;

/** @brief Pthread-backed static task storage. */
typedef struct host_static_task
{
    pthread_t thread;
    pthread_mutex_t notification_lock;
    pthread_cond_t notification_changed;
    uint32_t notification_count;
    void (*entry)(void *);
    void *context;
    UBaseType_t priority;
    uint32_t stack_depth;
    BaseType_t core_id;
    bool created;
    bool joinable;
    bool suspended;
    bool delete_requested;
    bool notification_initialized;
    bool notification_shutdown;
} StaticTask_t;

typedef StaticQueue_t *QueueHandle_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef StaticTask_t *TaskHandle_t;

typedef enum eTaskState
{
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid,
} eTaskState;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define tskNO_AFFINITY (-1)

#define configTICK_RATE_HZ  1000U
#define configMAX_PRIORITIES 25U

#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif /* __CONNECTIVITY_HOST_FREERTOS_H__ */

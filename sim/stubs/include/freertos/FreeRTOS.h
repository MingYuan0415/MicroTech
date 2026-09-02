/** @file Pthread-backed FreeRTOS types for the MicroTech simulator. */
#ifndef __SIM_FREERTOS_H__
#define __SIM_FREERTOS_H__

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
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
    const char *name;
    bool created;
    bool shutdown;
    bool dynamically_allocated;
    bool created_with_caps;
    bool suspended;
    UBaseType_t priority;
    UBaseType_t stack_memory_caps;
    BaseType_t core_id;
} StaticTask_t;

typedef StaticTask_t *TaskHandle_t;

/** @brief Pthread-backed queue; used both dynamically and statically. */
typedef struct host_queue
{
    uint8_t *items;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool owns_items;
    bool dynamic;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} StaticQueue_t;

/** @brief Opaque host queue handle. */
typedef StaticQueue_t *QueueHandle_t;

/** @brief Pthread-backed semaphore; used both dynamically and statically. */
typedef struct host_semaphore
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool available;
    bool recursive;
    bool dynamic;
    pthread_t owner;
    unsigned hold_count;
} StaticSemaphore_t;

/** @brief Opaque host semaphore handle. */
typedef StaticSemaphore_t *SemaphoreHandle_t;

/** @brief Opaque host event-group handle. */
typedef struct host_event_group *EventGroupHandle_t;

/** @brief Host representation of FreeRTOS event bits. */
typedef uint32_t EventBits_t;

/** @brief Critical-section guard; a recursive-hostel mutex stands in. */
typedef pthread_mutex_t portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#define BIT0 (UINT32_C(1) << 0)
#define BIT1 (UINT32_C(1) << 1)
#define BIT2 (UINT32_C(1) << 2)
#define BIT3 (UINT32_C(1) << 3)
#define BIT4 (UINT32_C(1) << 4)
#define BIT5 (UINT32_C(1) << 5)
#define BIT6 (UINT32_C(1) << 6)
#define BIT7 (UINT32_C(1) << 7)

#define tskIDLE_PRIORITY 0U
#define tskNO_AFFINITY (-1)
#define configTICK_RATE_HZ  1000U
#define configMAX_PRIORITIES 25U
#define configMINIMAL_STACK_SIZE 2048U
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(lock) ((void)pthread_mutex_lock((lock)))
#define taskEXIT_CRITICAL(lock) ((void)pthread_mutex_unlock((lock)))
#define taskYIELD() ((void)0)

#endif /* __SIM_FREERTOS_H__ */

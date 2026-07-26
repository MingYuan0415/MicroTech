#include "display_benchmark_host_port.h"

#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define HOST_TASK_CAPACITY 4U

typedef struct host_task
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint32_t notifications;
    bool deleted;
    const char *name;
    TaskFunction_t entry;
    void *arg;
} host_task_t;

typedef struct host_semaphore
{
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool given;
} host_semaphore_t;

static _Thread_local host_task_t *s_current_task;
static atomic_size_t s_fail_create_index;
static atomic_size_t s_create_count;
static atomic_size_t s_delete_count;
static uint32_t s_stack_depths[HOST_TASK_CAPACITY];
static unsigned s_stack_caps[HOST_TASK_CAPACITY];
static unsigned s_priorities[HOST_TASK_CAPACITY];
static const char *s_deleted_names[HOST_TASK_CAPACITY];

static struct timespec _host_deadline(TickType_t ticks)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(ticks / 1000U);
    deadline.tv_nsec += (long)(ticks % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void *_host_task_entry(void *arg)
{
    host_task_t *task = arg;
    s_current_task = task;
    task->entry(task->arg);
    s_current_task = NULL;
    return NULL;
}

void display_benchmark_host_port_reset(void)
{
    atomic_store(&s_fail_create_index, SIZE_MAX);
    atomic_store(&s_create_count, 0U);
    atomic_store(&s_delete_count, 0U);
    for (size_t index = 0U; index < HOST_TASK_CAPACITY; ++index)
    {
        s_stack_depths[index] = 0U;
        s_stack_caps[index] = 0U;
        s_priorities[index] = 0U;
        s_deleted_names[index] = NULL;
    }
}

void display_benchmark_host_port_fail_next_create(void)
{
    atomic_store(&s_fail_create_index, atomic_load(&s_create_count));
}

void display_benchmark_host_port_fail_create_on(size_t index)
{
    atomic_store(&s_fail_create_index, index);
}

size_t display_benchmark_host_port_create_count(void)
{
    return atomic_load(&s_create_count);
}

size_t display_benchmark_host_port_delete_count(void)
{
    return atomic_load(&s_delete_count);
}

uint32_t display_benchmark_host_port_stack_depth(size_t index)
{
    return index < HOST_TASK_CAPACITY ? s_stack_depths[index] : 0U;
}

unsigned display_benchmark_host_port_stack_caps(size_t index)
{
    return index < HOST_TASK_CAPACITY ? s_stack_caps[index] : 0U;
}

unsigned display_benchmark_host_port_priority(size_t index)
{
    return index < HOST_TASK_CAPACITY ? s_priorities[index] : 0U;
}

const char *display_benchmark_host_port_deleted_name(size_t index)
{
    return index < atomic_load(&s_delete_count) ? s_deleted_names[index] :
           NULL;
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t entry, const char *name,
                               uint32_t stack_depth, void *arg,
                               UBaseType_t priority, TaskHandle_t *task_handle,
                               UBaseType_t memory_caps)
{
    if (entry == NULL || task_handle == NULL)
    {
        return pdFAIL;
    }
    size_t fail_index = atomic_load(&s_fail_create_index);
    const size_t create_index = atomic_load(&s_create_count);
    if (create_index == fail_index &&
            atomic_compare_exchange_strong(&s_fail_create_index,
                                           &fail_index, SIZE_MAX))
    {
        return pdFAIL;
    }
    host_task_t *task = calloc(1, sizeof(*task));
    if (task == NULL)
    {
        return pdFAIL;
    }
    task->entry = entry;
    task->arg = arg;
    task->name = name;
    (void)pthread_mutex_init(&task->lock, NULL);
    (void)pthread_cond_init(&task->condition, NULL);
    const size_t index = atomic_fetch_add(&s_create_count, 1U);
    if (index >= HOST_TASK_CAPACITY ||
            pthread_create(&task->thread, NULL, _host_task_entry, task) != 0)
    {
        atomic_fetch_sub(&s_create_count, 1U);
        (void)pthread_cond_destroy(&task->condition);
        (void)pthread_mutex_destroy(&task->lock);
        free(task);
        return pdFAIL;
    }
    s_stack_depths[index] = stack_depth;
    s_stack_caps[index] = memory_caps;
    s_priorities[index] = priority;
    *task_handle = task;
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task_handle)
{
    host_task_t *task = task_handle;
    if (task == NULL)
    {
        return pdFAIL;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->notifications++;
    (void)pthread_cond_broadcast(&task->condition);
    (void)pthread_mutex_unlock(&task->lock);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks)
{
    host_task_t *task = s_current_task;
    if (task == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&task->lock);
    if (task->notifications == 0U && !task->deleted)
    {
        const struct timespec deadline = _host_deadline(timeout_ticks);
        (void)pthread_cond_timedwait(&task->condition, &task->lock,
                                     &deadline);
    }
    const uint32_t notifications = task->notifications;
    if (clear_on_exit)
    {
        task->notifications = 0U;
    }
    else if (task->notifications > 0U)
    {
        task->notifications--;
    }
    (void)pthread_mutex_unlock(&task->lock);
    return notifications;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    (void)usleep((useconds_t)ticks_to_delay * 1000U);
}

void vTaskSuspend(TaskHandle_t task_handle)
{
    host_task_t *task = task_handle == NULL ? s_current_task : task_handle;
    if (task == NULL)
    {
        return;
    }
    (void)pthread_mutex_lock(&task->lock);
    while (!task->deleted)
    {
        (void)pthread_cond_wait(&task->condition, &task->lock);
    }
    (void)pthread_mutex_unlock(&task->lock);
}

void vTaskDeleteWithCaps(TaskHandle_t task_handle)
{
    host_task_t *task = task_handle;
    if (task == NULL)
    {
        return;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->deleted = true;
    (void)pthread_cond_broadcast(&task->condition);
    (void)pthread_mutex_unlock(&task->lock);
    (void)pthread_join(task->thread, NULL);
    (void)pthread_cond_destroy(&task->condition);
    (void)pthread_mutex_destroy(&task->lock);
    const size_t delete_index = atomic_fetch_add(&s_delete_count, 1U);
    if (delete_index < HOST_TASK_CAPACITY)
    {
        s_deleted_names[delete_index] = task->name;
    }
    free(task);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    host_semaphore_t *semaphore = calloc(1, sizeof(*semaphore));
    if (semaphore != NULL)
    {
        (void)pthread_mutex_init(&semaphore->lock, NULL);
        (void)pthread_cond_init(&semaphore->condition, NULL);
    }
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore_handle,
                          TickType_t ticks)
{
    host_semaphore_t *semaphore = semaphore_handle;
    if (semaphore == NULL)
    {
        return pdFAIL;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    if (!semaphore->given && ticks == portMAX_DELAY)
    {
        while (!semaphore->given)
        {
            (void)pthread_cond_wait(&semaphore->condition, &semaphore->lock);
        }
    }
    else if (!semaphore->given && ticks != 0U)
    {
        const struct timespec deadline = _host_deadline(ticks);
        (void)pthread_cond_timedwait(&semaphore->condition, &semaphore->lock,
                                     &deadline);
    }
    const BaseType_t result = semaphore->given ? pdPASS : pdFAIL;
    semaphore->given = false;
    (void)pthread_mutex_unlock(&semaphore->lock);
    return result;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore_handle)
{
    host_semaphore_t *semaphore = semaphore_handle;
    if (semaphore == NULL)
    {
        return pdFAIL;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    semaphore->given = true;
    (void)pthread_cond_signal(&semaphore->condition);
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdPASS;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore_handle)
{
    host_semaphore_t *semaphore = semaphore_handle;
    if (semaphore != NULL)
    {
        (void)pthread_cond_destroy(&semaphore->condition);
        (void)pthread_mutex_destroy(&semaphore->lock);
        free(semaphore);
    }
}

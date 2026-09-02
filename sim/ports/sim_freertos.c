/** @file Simulator pthread-backed FreeRTOS port.
 *
 * Derived from tests/integration/host/host_freertos.c with the capacity and
 * API surface raised for the full firmware stack (~12-14 tasks, 6 queues,
 * ~35 semaphores, 5 event groups). Deliberate deviations from the base:
 *  - core affinity is recorded but never rejected (host is single-core;
 *    production pins the LVGL worker to core 1 and services to core 0);
 *  - static queue/semaphore storage and recursive mutexes were added;
 *  - a task-name registry backs xTaskGetHandle();
 *  - esp_timer runs real periodic/one-shot callbacks on a host thread.
 */
#include "sim_freertos.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SIM_FREERTOS_MAX_TASKS 32

struct host_event_group
{
    EventBits_t bits;
    pthread_mutex_t lock;
    pthread_cond_t changed;
};

struct host_timer
{
    void (*callback)(void *);
    void *arg;
    uint64_t period_us;
    bool periodic;
    pthread_t thread;
    pthread_mutex_t join_lock;
    bool has_thread;
    atomic_bool running;
};

static _Thread_local TaskHandle_t s_current_task;
static pthread_mutex_t s_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static TaskHandle_t s_tasks[SIM_FREERTOS_MAX_TASKS];
static size_t s_task_count;

typedef StaticQueue_t host_queue;
typedef StaticSemaphore_t host_semaphore;

static int _init_monotonic_condition(pthread_cond_t *condition)
{
    pthread_condattr_t attributes;
    int result = pthread_condattr_init(&attributes);
    if (result != 0)
    {
        return result;
    }
    result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (result == 0)
    {
        result = pthread_cond_init(condition, &attributes);
    }
    (void)pthread_condattr_destroy(&attributes);
    return result;
}

static struct timespec _deadline(TickType_t ticks)
{
    uint64_t nanoseconds = (uint64_t)ticks * UINT64_C(1000000);
    struct timespec result;
    (void)clock_gettime(CLOCK_MONOTONIC, &result);
    result.tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    result.tv_nsec += (long)(nanoseconds % UINT64_C(1000000000));
    if (result.tv_nsec >= 1000000000L)
    {
        ++result.tv_sec;
        result.tv_nsec -= 1000000000L;
    }
    return result;
}

static int _wait(pthread_cond_t *condition, pthread_mutex_t *lock,
                 TickType_t timeout, const struct timespec *deadline)
{
    if (timeout == portMAX_DELAY)
    {
        return pthread_cond_wait(condition, lock);
    }
    return pthread_cond_timedwait(condition, lock, deadline);
}

/* ---------------------------------------------------------------- queues */

static bool _queue_fill(host_queue *queue, uint32_t length,
                        uint32_t item_size, uint8_t *storage)
{
    memset(queue, 0, sizeof(*queue));
    if ((length == 0U) || (item_size == 0U))
    {
        return false;
    }
    if (storage != NULL)
    {
        queue->items = storage;
        queue->owns_items = false;
    }
    else
    {
        queue->items = calloc(length, item_size);
        if (queue->items == NULL)
        {
            return false;
        }
        queue->owns_items = true;
    }
    queue->item_size = item_size;
    queue->capacity = length;
    if (pthread_mutex_init(&queue->lock, NULL) != 0)
    {
        if (queue->owns_items)
        {
            free(queue->items);
        }
        return false;
    }
    if (_init_monotonic_condition(&queue->not_empty) != 0)
    {
        (void)pthread_mutex_destroy(&queue->lock);
        if (queue->owns_items)
        {
            free(queue->items);
        }
        return false;
    }
    if (_init_monotonic_condition(&queue->not_full) != 0)
    {
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->lock);
        if (queue->owns_items)
        {
            free(queue->items);
        }
        return false;
    }
    return true;
}

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    host_queue *queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
    {
        return NULL;
    }
    queue->dynamic = true;
    if (!_queue_fill(queue, length, item_size, NULL))
    {
        free(queue);
        return NULL;
    }
    return queue;
}

QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                 uint8_t *storage,
                                 StaticQueue_t *queue_storage)
{
    if ((queue_storage == NULL) || (storage == NULL) ||
            (queue_storage->capacity != 0U))
    {
        return NULL;
    }
    if (!_queue_fill(queue_storage, length, item_size, storage))
    {
        return NULL;
    }
    return queue_storage;
}

static BaseType_t _queue_send(QueueHandle_t queue, const void *item,
                              TickType_t timeout, bool front)
{
    if ((queue == NULL) || (item == NULL))
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if ((timeout != 0U) && (timeout != portMAX_DELAY))
    {
        deadline = _deadline(timeout);
    }
    while (queue->count == queue->capacity)
    {
        if ((timeout == 0U) ||
                (_wait(&queue->not_full, &queue->lock, timeout,
                       &deadline) == ETIMEDOUT))
        {
            (void)pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    if (front)
    {
        queue->tail = (queue->tail + queue->capacity - 1U) % queue->capacity;
        memcpy(queue->items + (queue->tail * queue->item_size), item,
               queue->item_size);
    }
    else
    {
        memcpy(queue->items + (queue->head * queue->item_size), item,
               queue->item_size);
        queue->head = (queue->head + 1U) % queue->capacity;
    }
    ++queue->count;
    (void)pthread_cond_signal(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t timeout)
{
    return _queue_send(queue, item, timeout, false);
}

BaseType_t xQueueSendToFront(QueueHandle_t queue, const void *item,
                             TickType_t timeout)
{
    return _queue_send(queue, item, timeout, true);
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *woken)
{
    const BaseType_t result = _queue_send(queue, item, 0U, false);
    if (woken != NULL)
    {
        *woken = pdFALSE;
    }
    return result;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    if ((queue == NULL) || (item == NULL))
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if ((timeout != 0U) && (timeout != portMAX_DELAY))
    {
        deadline = _deadline(timeout);
    }
    while (queue->count == 0U)
    {
        if ((timeout == 0U) ||
                (_wait(&queue->not_empty, &queue->lock, timeout,
                       &deadline) == ETIMEDOUT))
        {
            (void)pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    memcpy(item, queue->items + (queue->tail * queue->item_size),
           queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    --queue->count;
    (void)pthread_cond_signal(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    if (queue == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&queue->lock);
    const size_t count = queue->count;
    (void)pthread_mutex_unlock(&queue->lock);
    return (UBaseType_t)count;
}

void vQueueDelete(QueueHandle_t queue)
{
    if (queue == NULL)
    {
        return;
    }
    (void)pthread_cond_destroy(&queue->not_empty);
    (void)pthread_cond_destroy(&queue->not_full);
    (void)pthread_mutex_destroy(&queue->lock);
    if (queue->owns_items)
    {
        free(queue->items);
    }
    if (queue->dynamic)
    {
        free(queue);
    }
}

/* ------------------------------------------------------------ semaphores */

static SemaphoreHandle_t _semaphore_create(StaticSemaphore_t *storage,
        bool available, bool recursive)
{
    host_semaphore *semaphore = storage;

    if (semaphore == NULL)
    {
        semaphore = calloc(1, sizeof(*semaphore));
        if (semaphore == NULL)
        {
            return NULL;
        }
        semaphore->dynamic = true;
    }
    else
    {
        memset(semaphore, 0, sizeof(*semaphore));
    }
    if (pthread_mutex_init(&semaphore->lock, NULL) != 0)
    {
        if (semaphore->dynamic)
        {
            free(semaphore);
        }
        return NULL;
    }
    if (_init_monotonic_condition(&semaphore->changed) != 0)
    {
        (void)pthread_mutex_destroy(&semaphore->lock);
        if (semaphore->dynamic)
        {
            free(semaphore);
        }
        return NULL;
    }
    semaphore->available = available;
    semaphore->recursive = recursive;
    return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return _semaphore_create(NULL, false, false);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    if (storage == NULL)
    {
        return NULL;
    }
    return _semaphore_create(storage, false, false);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return _semaphore_create(NULL, true, false);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    if (storage == NULL)
    {
        return NULL;
    }
    return _semaphore_create(storage, true, false);
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return _semaphore_create(NULL, true, true);
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(
    StaticSemaphore_t *storage)
{
    if (storage == NULL)
    {
        return NULL;
    }
    return _semaphore_create(storage, true, true);
}

static BaseType_t _semaphore_take(SemaphoreHandle_t semaphore,
                                  TickType_t timeout, bool recursive)
{
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    if (recursive && semaphore->recursive && semaphore->hold_count > 0U &&
            pthread_equal(semaphore->owner, pthread_self()))
    {
        ++semaphore->hold_count;
        (void)pthread_mutex_unlock(&semaphore->lock);
        return pdTRUE;
    }
    struct timespec deadline = {0};
    if ((timeout != 0U) && (timeout != portMAX_DELAY))
    {
        deadline = _deadline(timeout);
    }
    while (!semaphore->available)
    {
        if ((timeout == 0U) ||
                (_wait(&semaphore->changed, &semaphore->lock, timeout,
                       &deadline) == ETIMEDOUT))
        {
            (void)pthread_mutex_unlock(&semaphore->lock);
            return pdFALSE;
        }
    }
    semaphore->available = false;
    if (recursive && semaphore->recursive)
    {
        semaphore->owner = pthread_self();
        semaphore->hold_count = 1U;
    }
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

static BaseType_t _semaphore_give(SemaphoreHandle_t semaphore, bool recursive)
{
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    if (recursive && semaphore->recursive)
    {
        if ((semaphore->hold_count == 0U) ||
                !pthread_equal(semaphore->owner, pthread_self()))
        {
            (void)pthread_mutex_unlock(&semaphore->lock);
            return pdFALSE;
        }
        if (--semaphore->hold_count > 0U)
        {
            (void)pthread_mutex_unlock(&semaphore->lock);
            return pdTRUE;
        }
    }
    semaphore->available = true;
    (void)pthread_cond_signal(&semaphore->changed);
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    return _semaphore_take(semaphore, timeout, false);
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore,
                                   TickType_t timeout)
{
    return _semaphore_take(semaphore, timeout, true);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return _semaphore_give(semaphore, false);
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore)
{
    return _semaphore_give(semaphore, true);
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL)
    {
        return;
    }
    (void)pthread_cond_destroy(&semaphore->changed);
    (void)pthread_mutex_destroy(&semaphore->lock);
    if (semaphore->dynamic)
    {
        free(semaphore);
    }
}

/* ---------------------------------------------------------- event groups */

EventGroupHandle_t xEventGroupCreate(void)
{
    struct host_event_group *group = calloc(1, sizeof(*group));
    if (group == NULL)
    {
        return NULL;
    }
    if (pthread_mutex_init(&group->lock, NULL) != 0)
    {
        free(group);
        return NULL;
    }
    if (_init_monotonic_condition(&group->changed) != 0)
    {
        (void)pthread_mutex_destroy(&group->lock);
        free(group);
        return NULL;
    }
    return group;
}

void vEventGroupDelete(EventGroupHandle_t group)
{
    if (group == NULL)
    {
        return;
    }
    (void)pthread_cond_destroy(&group->changed);
    (void)pthread_mutex_destroy(&group->lock);
    free(group);
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    if (group == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&group->lock);
    group->bits |= bits;
    const EventBits_t result = group->bits;
    (void)pthread_cond_broadcast(&group->changed);
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

EventBits_t xEventGroupSetBitsReturn(EventGroupHandle_t group,
                                     EventBits_t bits)
{
    return xEventGroupSetBits(group, bits);
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    if (group == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&group->lock);
    const EventBits_t result = group->bits;
    group->bits &= ~bits;
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t group)
{
    if (group == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&group->lock);
    const EventBits_t result = group->bits;
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

static bool _bits_ready(EventBits_t current, EventBits_t requested,
                        BaseType_t wait_for_all)
{
    return wait_for_all == pdTRUE ?
           (current & requested) == requested :
           (current & requested) != 0U;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all, TickType_t timeout)
{
    if ((group == NULL) || (bits == 0U))
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&group->lock);
    struct timespec deadline = {0};
    if ((timeout != 0U) && (timeout != portMAX_DELAY))
    {
        deadline = _deadline(timeout);
    }
    while (!_bits_ready(group->bits, bits, wait_for_all))
    {
        if ((timeout == 0U) ||
                (_wait(&group->changed, &group->lock, timeout,
                       &deadline) == ETIMEDOUT))
        {
            break;
        }
    }
    const EventBits_t result = group->bits;
    if (_bits_ready(result, bits, wait_for_all) && (clear_on_exit == pdTRUE))
    {
        group->bits &= ~bits;
    }
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

/* ----------------------------------------------------------------- tasks */

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return s_current_task;
}

static void _registry_add(TaskHandle_t task)
{
    (void)pthread_mutex_lock(&s_registry_lock);
    if (s_task_count < SIM_FREERTOS_MAX_TASKS)
    {
        s_tasks[s_task_count++] = task;
    }
    (void)pthread_mutex_unlock(&s_registry_lock);
}

static void _registry_remove(TaskHandle_t task)
{
    (void)pthread_mutex_lock(&s_registry_lock);
    for (size_t i = 0U; i < s_task_count; i++)
    {
        if (s_tasks[i] == task)
        {
            s_tasks[i] = s_tasks[s_task_count - 1U];
            s_tasks[--s_task_count] = NULL;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_registry_lock);
}

TaskHandle_t xTaskGetHandle(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }
    (void)pthread_mutex_lock(&s_registry_lock);
    TaskHandle_t found = NULL;
    for (size_t i = 0U; i < s_task_count; i++)
    {
        if ((s_tasks[i] != NULL) && (s_tasks[i]->name != NULL) &&
                (strcmp(s_tasks[i]->name, name) == 0))
        {
            found = s_tasks[i];
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_registry_lock);
    return found;
}

static void *_task_trampoline(void *context)
{
    TaskHandle_t task = context;
    s_current_task = task;
    task->entry(task->context);
    if (task->dynamically_allocated)
    {
        vTaskDelete(NULL);
    }
    s_current_task = NULL;
    return NULL;
}

static void _task_sync_destroy(TaskHandle_t task)
{
    (void)pthread_cond_destroy(&task->notification_ready);
    (void)pthread_mutex_destroy(&task->lock);
}

static bool _task_start(TaskHandle_t task, void (*entry)(void *),
                        void *context, const char *name,
                        UBaseType_t priority, bool dynamically_allocated)
{
    if (pthread_mutex_init(&task->lock, NULL) != 0)
    {
        return false;
    }
    if (_init_monotonic_condition(&task->notification_ready) != 0)
    {
        (void)pthread_mutex_destroy(&task->lock);
        return false;
    }
    task->notification = 0U;
    task->entry = entry;
    task->context = context;
    task->name = name;
    task->priority = priority;
    task->shutdown = false;
    task->suspended = false;
    task->dynamically_allocated = dynamically_allocated;
    task->created = true;
    _registry_add(task);
    if (pthread_create(&task->thread, NULL, _task_trampoline, task) != 0)
    {
        task->created = false;
        _registry_remove(task);
        _task_sync_destroy(task);
        return false;
    }
    return true;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *task_storage, BaseType_t core_id)
{
    (void)stack_depth;
    (void)stack;
    if ((entry == NULL) || (task_storage == NULL) || task_storage->created)
    {
        return NULL;
    }
    task_storage->core_id = core_id;
    task_storage->dynamically_allocated = false;
    task_storage->created_with_caps = false;
    if (!_task_start(task_storage, entry, context, name, priority, false))
    {
        return NULL;
    }
    return task_storage;
}

static BaseType_t _task_create_dynamic(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *out_task,
    bool with_caps, UBaseType_t memory_caps, BaseType_t core_id)
{
    (void)stack_depth;
    if ((entry == NULL) || (out_task == NULL))
    {
        return pdFAIL;
    }
    *out_task = NULL;
    TaskHandle_t task = calloc(1, sizeof(*task));
    if (task == NULL)
    {
        return pdFAIL;
    }
    task->created_with_caps = with_caps;
    task->stack_memory_caps = memory_caps;
    task->core_id = core_id;
    if (!_task_start(task, entry, context, name, priority, true))
    {
        free(task);
        return pdFAIL;
    }
    *out_task = task;
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *out_task,
    BaseType_t core_id)
{
    return _task_create_dynamic(entry, name, stack_depth, context, priority,
                                out_task, false, 0U, core_id);
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *out_task,
    BaseType_t core_id, UBaseType_t memory_caps)
{
    return _task_create_dynamic(entry, name, stack_depth, context, priority,
                                out_task, true, memory_caps, core_id);
}

BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_depth, void *context,
                       UBaseType_t priority, TaskHandle_t *out_task)
{
    return _task_create_dynamic(entry, name, stack_depth, context, priority,
                                out_task, false, 0U, tskNO_AFFINITY);
}

BaseType_t xTaskGetCoreID(TaskHandle_t task)
{
    return (task == NULL) ? tskNO_AFFINITY : task->core_id;
}

void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority)
{
    if (task != NULL)
    {
        task->priority = priority;
    }
}

void vTaskSuspend(TaskHandle_t task)
{
    if (task != NULL)
    {
        (void)pthread_mutex_lock(&task->lock);
        task->suspended = true;
        (void)pthread_mutex_unlock(&task->lock);
    }
}

void vTaskResume(TaskHandle_t task)
{
    if (task != NULL)
    {
        (void)pthread_mutex_lock(&task->lock);
        task->suspended = false;
        (void)pthread_cond_broadcast(&task->notification_ready);
        (void)pthread_mutex_unlock(&task->lock);
    }
}

eTaskState eTaskGetState(TaskHandle_t task)
{
    bool suspended;

    if (task == NULL)
    {
        return eDeleted;
    }
    if (!task->created)
    {
        return eDeleted;
    }
    (void)pthread_mutex_lock(&task->lock);
    suspended = task->suspended;
    (void)pthread_mutex_unlock(&task->lock);
    return suspended ? eSuspended : eRunning;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096U;
}

uint8_t *xTaskGetStackStart(TaskHandle_t task)
{
    (void)task;
    return NULL;
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action)
{
    if (task == NULL)
    {
        return pdFAIL;
    }
    (void)pthread_mutex_lock(&task->lock);
    if (task->shutdown)
    {
        (void)pthread_mutex_unlock(&task->lock);
        return pdFAIL;
    }
    switch (action)
    {
    case eSetBits:
        task->notification |= value;
        break;
    case eIncrement:
        ++task->notification;
        break;
    case eSetValueWithOverwrite:
        task->notification = value;
        break;
    case eSetValueWithoutOverwrite:
        if (task->notification != 0U)
        {
            (void)pthread_mutex_unlock(&task->lock);
            return pdFAIL;
        }
        task->notification = value;
        break;
    case eNoAction:
    default:
        break;
    }
    (void)pthread_cond_signal(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout)
{
    TaskHandle_t task = s_current_task;
    if (task == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->notification &= ~clear_on_entry;
    struct timespec deadline = {0};
    if ((timeout != 0U) && (timeout != portMAX_DELAY))
    {
        deadline = _deadline(timeout);
    }
    while ((task->notification == 0U) && !task->shutdown && !task->suspended)
    {
        if ((timeout == 0U) ||
                (_wait(&task->notification_ready, &task->lock, timeout,
                       &deadline) == ETIMEDOUT))
        {
            (void)pthread_mutex_unlock(&task->lock);
            return pdFALSE;
        }
    }
    if (task->shutdown)
    {
        (void)pthread_mutex_unlock(&task->lock);
        pthread_exit(NULL);
    }
    if (value != NULL)
    {
        *value = task->notification;
    }
    task->notification &= ~clear_on_exit;
    (void)pthread_mutex_unlock(&task->lock);
    return pdTRUE;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    return xTaskNotify(task, 1U, eIncrement);
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout)
{
    TaskHandle_t task = s_current_task;
    if (task == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&task->lock);
    struct timespec deadline = {0};
    if ((timeout != 0U) && (timeout != portMAX_DELAY))
    {
        deadline = _deadline(timeout);
    }
    while ((task->notification == 0U) && !task->shutdown)
    {
        if ((timeout == 0U) ||
                (_wait(&task->notification_ready, &task->lock, timeout,
                       &deadline) == ETIMEDOUT))
        {
            (void)pthread_mutex_unlock(&task->lock);
            return 0U;
        }
    }
    if (task->shutdown)
    {
        (void)pthread_mutex_unlock(&task->lock);
        pthread_exit(NULL);
    }
    const uint32_t result = task->notification;
    if (clear_on_exit == pdTRUE)
    {
        task->notification = 0U;
    }
    else
    {
        --task->notification;
    }
    (void)pthread_mutex_unlock(&task->lock);
    return result;
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec current;
    (void)clock_gettime(CLOCK_MONOTONIC, &current);
    const uint64_t ticks = (uint64_t)current.tv_sec * configTICK_RATE_HZ +
                           (((uint64_t)current.tv_nsec * configTICK_RATE_HZ) /
                            UINT64_C(1000000000));
    return (TickType_t)ticks;
}

static void _delay_plain(TickType_t ticks)
{
    if (ticks == 0U)
    {
        return;
    }
    const uint64_t nanoseconds =
        ((uint64_t)ticks * UINT64_C(1000000000)) / configTICK_RATE_HZ;
    const struct timespec delay =
    {
        .tv_sec = (time_t)(nanoseconds / UINT64_C(1000000000)),
        .tv_nsec = (long)(nanoseconds % UINT64_C(1000000000)),
    };
    (void)nanosleep(&delay, NULL);
}

void vTaskDelay(TickType_t ticks)
{
    TaskHandle_t task = s_current_task;
    if (task == NULL)
    {
        _delay_plain(ticks);
        return;
    }
    (void)pthread_mutex_lock(&task->lock);
    if (ticks == portMAX_DELAY)
    {
        while (!task->shutdown)
        {
            (void)pthread_cond_wait(&task->notification_ready, &task->lock);
        }
    }
    else if (ticks != 0U)
    {
        const struct timespec deadline = _deadline(ticks);
        while (!task->shutdown &&
                pthread_cond_timedwait(&task->notification_ready, &task->lock,
                                       &deadline) != ETIMEDOUT)
        {
        }
    }
    const bool shutdown = task->shutdown;
    (void)pthread_mutex_unlock(&task->lock);
    if (shutdown)
    {
        pthread_exit(NULL);
    }
}

void vTaskDelete(TaskHandle_t task)
{
    const bool self_delete = (task == NULL) || (task == s_current_task);
    if (self_delete)
    {
        TaskHandle_t current = s_current_task;
        if (current == NULL)
        {
            return;
        }
        s_current_task = NULL;
        if (current->dynamically_allocated)
        {
            _registry_remove(current);
            (void)pthread_detach(pthread_self());
            _task_sync_destroy(current);
            free(current);
        }
        pthread_exit(NULL);
    }
    if (!task->dynamically_allocated)
    {
        return;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->shutdown = true;
    (void)pthread_cond_broadcast(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    (void)pthread_join(task->thread, NULL);
    _registry_remove(task);
    _task_sync_destroy(task);
    free(task);
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    vTaskDelete(task);
}

/* ----------------------------------------------------------------- timers */

static void *_timer_trampoline(void *context)
{
    struct host_timer *timer = context;
    if (!timer->periodic)
    {
        usleep((useconds_t)timer->period_us);
        if (atomic_load(&timer->running))
        {
            timer->callback(timer->arg);
        }
        atomic_store(&timer->running, false);
        return NULL;
    }
    while (atomic_load(&timer->running))
    {
        usleep((useconds_t)timer->period_us);
        if (atomic_load(&timer->running) && (timer->callback != NULL))
        {
            timer->callback(timer->arg);
        }
    }
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out_handle)
{
    if ((args == NULL) || (args->callback == NULL) || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    struct host_timer *created = calloc(1, sizeof(*created));
    if (created == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    created->callback = args->callback;
    created->arg = args->arg;
    if (pthread_mutex_init(&created->join_lock, NULL) != 0)
    {
        free(created);
        return ESP_FAIL;
    }
    *out_handle = created;
    return ESP_OK;
}

static esp_err_t _timer_start(esp_timer_handle_t timer, uint64_t period_us,
                              bool periodic)
{
    if ((timer == NULL) || (period_us == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&timer->running))
    {
        return ESP_ERR_INVALID_STATE;
    }
    timer->period_us = period_us;
    timer->periodic = periodic;
    (void)pthread_mutex_lock(&timer->join_lock);
    if (timer->has_thread)
    {
        if (atomic_load(&timer->running))
        {
            (void)pthread_mutex_unlock(&timer->join_lock);
            return ESP_ERR_INVALID_STATE;
        }
        (void)pthread_join(timer->thread, NULL);
        timer->has_thread = false;
    }
    atomic_store(&timer->running, true);
    if (pthread_create(&timer->thread, NULL, _timer_trampoline, timer) != 0)
    {
        atomic_store(&timer->running, false);
        (void)pthread_mutex_unlock(&timer->join_lock);
        return ESP_FAIL;
    }
    timer->has_thread = true;
    (void)pthread_mutex_unlock(&timer->join_lock);
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,
                                   uint64_t period_us)
{
    return _timer_start(timer, period_us, true);
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay_us)
{
    return _timer_start(timer, delay_us, false);
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&timer->running, false);
    (void)pthread_mutex_lock(&timer->join_lock);
    if (timer->has_thread)
    {
        (void)pthread_join(timer->thread, NULL);
        timer->has_thread = false;
    }
    (void)pthread_mutex_unlock(&timer->join_lock);
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)esp_timer_stop(timer);
    (void)pthread_mutex_destroy(&timer->join_lock);
    free(timer);
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return ((int64_t)now.tv_sec * 1000000) + (now.tv_nsec / 1000);
}

unsigned long long sim_log_uptime_ms(void)
{
    return (unsigned long long)(esp_timer_get_time() / 1000);
}

uint32_t esp_random(void)
{
    static uint64_t state = UINT64_C(0x9E3779B97F4A7C15);
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (uint32_t)(state >> 16);
}

void esp_fill_random(void *buf, size_t n)
{
    uint8_t *bytes = buf;
    for (size_t i = 0; i < n; i++)
    {
        bytes[i] = (uint8_t)(esp_random() >> ((i & 3U) * 8U));
    }
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(count, size);
}

void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps)
{
    void *result = NULL;

    (void)caps;
    /* posix_memalign needs a power-of-two alignment at least as wide as a
     * pointer; IDF's heap_caps tolerates anything. */
    size_t realign = (align < sizeof(void *)) ? sizeof(void *) : align;
    while ((realign & (realign - 1U)) != 0U)
    {
        realign += realign;
    }
    if (posix_memalign(&result, realign, size) != 0)
    {
        return NULL;
    }
    return result;
}

void *heap_caps_aligned_calloc(size_t align, size_t count, size_t size,
                               uint32_t caps)
{
    void *result = heap_caps_aligned_alloc(align, count * size, caps);
    if (result != NULL)
    {
        memset(result, 0, count * size);
    }
    return result;
}

void heap_caps_free(void *memory)
{
    free(memory);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return SIZE_MAX / 4U;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return SIZE_MAX / 8U;
}

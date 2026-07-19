#include "host_freertos.h"

#include "apps_integration_runtime.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct host_queue
{
    uint8_t *items;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

struct host_semaphore
{
    bool available;
    pthread_mutex_t lock;
    pthread_cond_t changed;
};

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
    pthread_t thread;
    pthread_mutex_t state_lock;
    pthread_cond_t state_changed;
    bool callback_running;
    atomic_bool running;
    atomic_bool paused;
    atomic_uint pending_ticks;
};

static _Thread_local TaskHandle_t s_current_static_task;
static _Thread_local unsigned char s_thread_token;
static pthread_mutex_t s_task_state_lock = PTHREAD_MUTEX_INITIALIZER;
static TaskHandle_t s_created_task;
static atomic_size_t s_dynamic_task_count;
static atomic_size_t s_caps_task_count;
static atomic_size_t s_caps_task_owner_delete_count;
static atomic_size_t s_caps_task_wrong_delete_count;
static atomic_size_t s_caps_task_self_delete_count;
static atomic_uint s_last_task_stack_caps;

static int _host_init_monotonic_condition(pthread_cond_t *condition)
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

static struct timespec _host_deadline(TickType_t ticks)
{
    uint64_t nanoseconds = (uint64_t)ticks * UINT64_C(1000000);
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    deadline.tv_nsec += (long)(nanoseconds % UINT64_C(1000000000));
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static int _host_wait(pthread_cond_t *condition, pthread_mutex_t *lock,
                      TickType_t timeout, struct timespec *deadline)
{
    if (timeout == portMAX_DELAY)
    {
        return pthread_cond_wait(condition, lock);
    }
    return pthread_cond_timedwait(condition, lock, deadline);
}

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    if (length == 0 || item_size == 0)
    {
        return NULL;
    }
    struct host_queue *queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
    {
        return NULL;
    }
    queue->items = calloc(length, item_size);
    if (queue->items == NULL)
    {
        free(queue);
        return NULL;
    }
    queue->item_size = item_size;
    queue->capacity = length;
    if (pthread_mutex_init(&queue->lock, NULL) != 0)
    {
        free(queue->items);
        free(queue);
        return NULL;
    }
    if (_host_init_monotonic_condition(&queue->not_empty) != 0)
    {
        (void)pthread_mutex_destroy(&queue->lock);
        free(queue->items);
        free(queue);
        return NULL;
    }
    if (_host_init_monotonic_condition(&queue->not_full) != 0)
    {
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->lock);
        free(queue->items);
        free(queue);
        return NULL;
    }
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t timeout)
{
    if (queue == NULL || item == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (queue->count == queue->capacity)
    {
        if (timeout == 0 || _host_wait(&queue->not_full, &queue->lock,
                                       timeout, &deadline) == ETIMEDOUT)
        {
            (void)pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    memcpy(queue->items + queue->head * queue->item_size,
           item, queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    ++queue->count;
    (void)pthread_cond_signal(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueSendToFront(QueueHandle_t queue, const void *item,
                             TickType_t timeout)
{
    if (queue == NULL || item == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (queue->count == queue->capacity)
    {
        if (timeout == 0 || _host_wait(&queue->not_full, &queue->lock,
                                       timeout, &deadline) == ETIMEDOUT)
        {
            (void)pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    queue->tail = (queue->tail + queue->capacity - 1U) % queue->capacity;
    memcpy(queue->items + queue->tail * queue->item_size,
           item, queue->item_size);
    ++queue->count;
    (void)pthread_cond_signal(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t timeout)
{
    if (queue == NULL || item == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (queue->count == 0)
    {
        if (timeout == 0 || _host_wait(&queue->not_empty, &queue->lock,
                                       timeout, &deadline) == ETIMEDOUT)
        {
            (void)pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    memcpy(item, queue->items + queue->tail * queue->item_size,
           queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    --queue->count;
    (void)pthread_cond_signal(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
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
    free(queue->items);
    free(queue);
}

static SemaphoreHandle_t _host_semaphore_create(bool available)
{
    struct host_semaphore *semaphore = calloc(1, sizeof(*semaphore));
    if (semaphore == NULL)
    {
        return NULL;
    }
    semaphore->available = available;
    if (pthread_mutex_init(&semaphore->lock, NULL) != 0)
    {
        free(semaphore);
        return NULL;
    }
    if (_host_init_monotonic_condition(&semaphore->changed) != 0)
    {
        (void)pthread_mutex_destroy(&semaphore->lock);
        free(semaphore);
        return NULL;
    }
    return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return _host_semaphore_create(false);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return _host_semaphore_create(true);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (!semaphore->available)
    {
        if (timeout == 0 || _host_wait(&semaphore->changed, &semaphore->lock,
                                       timeout, &deadline) == ETIMEDOUT)
        {
            (void)pthread_mutex_unlock(&semaphore->lock);
            return pdFALSE;
        }
    }
    semaphore->available = false;
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    semaphore->available = true;
    (void)pthread_cond_signal(&semaphore->changed);
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL)
    {
        return;
    }
    (void)pthread_cond_destroy(&semaphore->changed);
    (void)pthread_mutex_destroy(&semaphore->lock);
    free(semaphore);
}

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
    if (_host_init_monotonic_condition(&group->changed) != 0)
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

static bool _host_event_bits_ready(EventBits_t current,
                                   EventBits_t requested,
                                   BaseType_t wait_for_all)
{
    return wait_for_all == pdTRUE ?
           (current & requested) == requested :
           (current & requested) != 0U;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                EventBits_t bits,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all,
                                TickType_t timeout)
{
    if (group == NULL || bits == 0U)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&group->lock);
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (!_host_event_bits_ready(group->bits, bits, wait_for_all))
    {
        if (timeout == 0 || _host_wait(&group->changed, &group->lock,
                                       timeout, &deadline) == ETIMEDOUT)
        {
            break;
        }
    }
    const EventBits_t result = group->bits;
    if (_host_event_bits_ready(result, bits, wait_for_all) &&
            clear_on_exit == pdTRUE)
    {
        group->bits &= ~bits;
    }
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    if (s_current_static_task != NULL)
    {
        return s_current_static_task;
    }
    return (TaskHandle_t)(void *)&s_thread_token;
}

static void *_host_task_trampoline(void *context)
{
    TaskHandle_t task = context;
    s_current_static_task = task;
    task->entry(task->context);
    if (task->dynamically_allocated)
    {
        vTaskDelete(NULL);
    }
    s_current_static_task = NULL;
    return NULL;
}

static void _host_task_sync_destroy(TaskHandle_t task)
{
    (void)pthread_cond_destroy(&task->notification_ready);
    (void)pthread_mutex_destroy(&task->lock);
}

static bool _host_task_start(TaskHandle_t task, void (*entry)(void *),
                             void *context, bool dynamically_allocated)
{
    if (pthread_mutex_init(&task->lock, NULL) != 0)
    {
        return false;
    }
    if (_host_init_monotonic_condition(&task->notification_ready) != 0)
    {
        (void)pthread_mutex_destroy(&task->lock);
        return false;
    }
    task->notification = 0U;
    task->entry = entry;
    task->context = context;
    task->shutdown = false;
    task->dynamically_allocated = dynamically_allocated;
    task->created = true;
    if (pthread_create(&task->thread, NULL, _host_task_trampoline, task) != 0)
    {
        task->created = false;
        _host_task_sync_destroy(task);
        return false;
    }
    return true;
}

TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *context,
                               UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *task_storage)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)stack;
    if (entry == NULL || task_storage == NULL || task_storage->created)
    {
        return NULL;
    }
    if (!_host_task_start(task_storage, entry, context, false))
    {
        return NULL;
    }
    (void)pthread_mutex_lock(&s_task_state_lock);
    s_created_task = task_storage;
    (void)pthread_mutex_unlock(&s_task_state_lock);
    return task_storage;
}

static BaseType_t _host_task_create_dynamic(
    void (*entry)(void *), const char *name,
    uint32_t stack_depth, void *context,
    UBaseType_t priority, TaskHandle_t *out_task,
    bool with_caps, UBaseType_t memory_caps)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    if (entry == NULL || out_task == NULL)
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
    atomic_fetch_add_explicit(&s_dynamic_task_count, 1U,
                              memory_order_relaxed);
    if (with_caps)
    {
        atomic_fetch_add_explicit(&s_caps_task_count, 1U,
                                  memory_order_relaxed);
        atomic_store_explicit(&s_last_task_stack_caps, memory_caps,
                              memory_order_relaxed);
    }
    if (!_host_task_start(task, entry, context, true))
    {
        atomic_fetch_sub_explicit(&s_dynamic_task_count, 1U,
                                  memory_order_relaxed);
        if (with_caps)
        {
            atomic_fetch_sub_explicit(&s_caps_task_count, 1U,
                                      memory_order_relaxed);
        }
        free(task);
        return pdFAIL;
    }
    *out_task = task;
    return pdPASS;
}

BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_depth, void *context,
                       UBaseType_t priority, TaskHandle_t *out_task)
{
    return _host_task_create_dynamic(entry, name, stack_depth, context,
                                     priority, out_task, false, 0U);
}

BaseType_t xTaskCreateWithCaps(void (*entry)(void *), const char *name,
                               uint32_t stack_depth, void *context,
                               UBaseType_t priority, TaskHandle_t *out_task,
                               UBaseType_t memory_caps)
{
    return _host_task_create_dynamic(entry, name, stack_depth, context,
                                     priority, out_task, true, memory_caps);
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action)
{
    if (task == NULL || action != eSetBits)
    {
        return pdFAIL;
    }
    (void)pthread_mutex_lock(&task->lock);
    if (task->shutdown)
    {
        (void)pthread_mutex_unlock(&task->lock);
        return pdFAIL;
    }
    task->notification |= value;
    (void)pthread_cond_signal(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout)
{
    TaskHandle_t task = s_current_static_task;
    if (task == NULL)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->notification &= ~clear_on_entry;
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (task->notification == 0 && !task->shutdown)
    {
        if (timeout == 0 || _host_wait(&task->notification_ready, &task->lock,
                                       timeout, &deadline) == ETIMEDOUT)
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
    if (task->notification != UINT32_MAX)
    {
        ++task->notification;
    }
    (void)pthread_cond_signal(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout)
{
    TaskHandle_t task = s_current_static_task;
    if (task == NULL)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&task->lock);
    struct timespec deadline = {0};
    if (timeout != 0 && timeout != portMAX_DELAY)
    {
        deadline = _host_deadline(timeout);
    }
    while (task->notification == 0U && !task->shutdown)
    {
        if (timeout == 0 || _host_wait(&task->notification_ready, &task->lock,
                                       timeout, &deadline) == ETIMEDOUT)
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
                           ((uint64_t)current.tv_nsec * configTICK_RATE_HZ) /
                           UINT64_C(1000000000);
    return (TickType_t)ticks;
}

static void _host_task_delay_without_handle(TickType_t ticks)
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
    TaskHandle_t task = s_current_static_task;
    if (task == NULL)
    {
        _host_task_delay_without_handle(ticks);
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
        const struct timespec deadline = _host_deadline(ticks);
        while (!task->shutdown &&
                pthread_cond_timedwait(&task->notification_ready,
                                       &task->lock, &deadline) != ETIMEDOUT)
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

static void _host_task_record_delete(TaskHandle_t task, bool self_delete,
                                     bool with_caps_api)
{
    if (task == NULL || !task->dynamically_allocated)
    {
        return;
    }
    if (task->created_with_caps)
    {
        if (self_delete)
        {
            atomic_fetch_add_explicit(&s_caps_task_self_delete_count, 1U,
                                      memory_order_relaxed);
        }
        if (!with_caps_api)
        {
            atomic_fetch_add_explicit(&s_caps_task_wrong_delete_count, 1U,
                                      memory_order_relaxed);
        }
        else if (!self_delete)
        {
            atomic_fetch_add_explicit(&s_caps_task_owner_delete_count, 1U,
                                      memory_order_relaxed);
        }
    }
    else if (with_caps_api)
    {
        atomic_fetch_add_explicit(&s_caps_task_wrong_delete_count, 1U,
                                  memory_order_relaxed);
    }
}

static void _host_task_delete(TaskHandle_t task, bool with_caps_api)
{
    TaskHandle_t current = s_current_static_task;
    const bool self_delete = task == NULL || task == current;
    TaskHandle_t target = self_delete ? current : task;
    _host_task_record_delete(target, self_delete, with_caps_api);
    if (self_delete)
    {
        if (current == NULL)
        {
            return;
        }
        s_current_static_task = NULL;
        if (current->dynamically_allocated)
        {
            atomic_fetch_sub_explicit(&s_dynamic_task_count, 1U,
                                      memory_order_relaxed);
            if (current->created_with_caps)
            {
                atomic_fetch_sub_explicit(&s_caps_task_count, 1U,
                                          memory_order_relaxed);
            }
            (void)pthread_detach(pthread_self());
            _host_task_sync_destroy(current);
            free(current);
        }
        pthread_exit(NULL);
    }
    if (!target->dynamically_allocated)
    {
        return;
    }
    (void)pthread_mutex_lock(&target->lock);
    target->shutdown = true;
    (void)pthread_cond_broadcast(&target->notification_ready);
    (void)pthread_mutex_unlock(&target->lock);
    (void)pthread_join(target->thread, NULL);
    atomic_fetch_sub_explicit(&s_dynamic_task_count, 1U,
                              memory_order_relaxed);
    if (target->created_with_caps)
    {
        atomic_fetch_sub_explicit(&s_caps_task_count, 1U,
                                  memory_order_relaxed);
    }
    _host_task_sync_destroy(target);
    free(target);
}

void vTaskDelete(TaskHandle_t task)
{
    _host_task_delete(task, false);
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    _host_task_delete(task, true);
}

size_t host_dynamic_task_count(void)
{
    return atomic_load_explicit(&s_dynamic_task_count,
                                memory_order_relaxed);
}

size_t host_caps_task_count(void)
{
    return atomic_load_explicit(&s_caps_task_count, memory_order_relaxed);
}

UBaseType_t host_last_task_stack_caps(void)
{
    return atomic_load_explicit(&s_last_task_stack_caps,
                                memory_order_relaxed);
}

size_t host_caps_task_owner_delete_count(void)
{
    return atomic_load_explicit(&s_caps_task_owner_delete_count,
                                memory_order_relaxed);
}

size_t host_caps_task_wrong_delete_count(void)
{
    return atomic_load_explicit(&s_caps_task_wrong_delete_count,
                                memory_order_relaxed);
}

size_t host_caps_task_self_delete_count(void)
{
    return atomic_load_explicit(&s_caps_task_self_delete_count,
                                memory_order_relaxed);
}

void host_task_shutdown(void)
{
    (void)pthread_mutex_lock(&s_task_state_lock);
    TaskHandle_t task = s_created_task;
    s_created_task = NULL;
    (void)pthread_mutex_unlock(&s_task_state_lock);
    if (task == NULL)
    {
        return;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->shutdown = true;
    (void)pthread_cond_broadcast(&task->notification_ready);
    (void)pthread_mutex_unlock(&task->lock);
    (void)pthread_join(task->thread, NULL);
    (void)pthread_cond_destroy(&task->notification_ready);
    (void)pthread_mutex_destroy(&task->lock);
}

static bool _host_timer_take_tick(struct host_timer *timer)
{
    unsigned pending = atomic_load(&timer->pending_ticks);
    while (pending > 0)
    {
        if (atomic_compare_exchange_weak(&timer->pending_ticks, &pending,
                                         pending - 1U))
        {
            return true;
        }
    }
    return false;
}

static void *_host_timer_trampoline(void *context)
{
    struct host_timer *timer = context;
    while (atomic_load(&timer->running))
    {
        usleep((useconds_t)timer->period_us);
        (void)pthread_mutex_lock(&timer->state_lock);
        const bool stepped = _host_timer_take_tick(timer);
        const bool fire = atomic_load(&timer->running) &&
                          (!atomic_load(&timer->paused) ||
                           stepped);
        timer->callback_running = fire;
        (void)pthread_mutex_unlock(&timer->state_lock);
        if (fire && timer->callback != NULL)
        {
            timer->callback(timer->arg);
        }
        if (fire)
        {
            (void)pthread_mutex_lock(&timer->state_lock);
            timer->callback_running = false;
            (void)pthread_cond_broadcast(&timer->state_changed);
            (void)pthread_mutex_unlock(&timer->state_lock);
        }
    }
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *timer)
{
    if (args == NULL || args->callback == NULL || timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    struct host_timer *created = calloc(1, sizeof(*created));
    if (created == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    if (pthread_mutex_init(&created->state_lock, NULL) != 0)
    {
        free(created);
        return ESP_FAIL;
    }
    if (pthread_cond_init(&created->state_changed, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&created->state_lock);
        free(created);
        return ESP_FAIL;
    }
    created->callback = args->callback;
    created->arg = args->arg;
    *timer = created;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer,
                                   uint64_t period_us)
{
    if (timer == NULL || period_us == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    timer->period_us = period_us;
    atomic_store(&timer->running, true);
    if (pthread_create(&timer->thread, NULL, _host_timer_trampoline, timer) != 0)
    {
        atomic_store(&timer->running, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&timer->running, false))
    {
        (void)pthread_join(timer->thread, NULL);
    }
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)esp_timer_stop(timer);
    (void)pthread_cond_destroy(&timer->state_changed);
    (void)pthread_mutex_destroy(&timer->state_lock);
    free(timer);
    return ESP_OK;
}

void host_timer_set_paused(esp_timer_handle_t timer, bool paused)
{
    if (timer != NULL)
    {
        (void)pthread_mutex_lock(&timer->state_lock);
        atomic_store(&timer->paused, paused);
        if (paused)
        {
            atomic_store(&timer->pending_ticks, 0U);
        }
        while (paused && timer->callback_running)
        {
            (void)pthread_cond_wait(&timer->state_changed,
                                    &timer->state_lock);
        }
        (void)pthread_mutex_unlock(&timer->state_lock);
    }
}

void host_timer_step(esp_timer_handle_t timer)
{
    if (timer != NULL)
    {
        atomic_fetch_add(&timer->pending_ticks, 1U);
    }
}

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_freertos.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static _Thread_local TaskHandle_t s_current_task;
static _Thread_local unsigned char s_external_task_token;

static pthread_mutex_t s_control_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_control_changed = PTHREAD_COND_INITIALIZER;
#define HOST_MAX_QUEUES 4U
static QueueHandle_t s_queues[HOST_MAX_QUEUES];
static bool s_queue_receive_paused;
static bool s_queue_receive_waiting;
static bool s_queue_delivery_deferred;
static bool s_queue_delivery_deferred_observed;
static unsigned s_semaphore_creates_before_failure = UINT_MAX;
static unsigned s_fail_queue_creates;
static unsigned s_fail_task_creates;
static unsigned s_fail_queue_sends;
static bool s_block_next_binary_give;
static bool s_binary_give_blocked;
static bool s_binary_give_released;
static unsigned s_live_semaphores;
static unsigned s_live_queues;
static unsigned s_live_tasks;
static unsigned s_total_semaphore_creates;
static unsigned s_total_queue_creates;
static unsigned s_total_task_creates;
static atomic_uint_fast64_t s_virtual_ticks;
static struct timespec s_tick_origin;
static pthread_once_t s_tick_once = PTHREAD_ONCE_INIT;

static void _host_init_tick_origin(void)
{
    (void)clock_gettime(CLOCK_MONOTONIC, &s_tick_origin);
}

static struct timespec _host_deadline_after_ms(uint32_t timeout_ms)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static struct timespec _host_deadline_after_ticks(TickType_t ticks)
{
    uint64_t milliseconds =
        ((uint64_t)ticks * UINT64_C(1000)) / configTICK_RATE_HZ;
    if (milliseconds == 0 && ticks != 0)
    {
        milliseconds = 1;
    }
    if (milliseconds > UINT32_MAX)
    {
        milliseconds = UINT32_MAX;
    }
    return _host_deadline_after_ms((uint32_t)milliseconds);
}

static bool _host_consume(unsigned *counter)
{
    bool consume = false;
    (void)pthread_mutex_lock(&s_control_lock);
    if (*counter > 0)
    {
        --*counter;
        consume = true;
    }
    (void)pthread_mutex_unlock(&s_control_lock);
    return consume;
}

static bool _host_fail_semaphore_create(void)
{
    bool fail = false;
    (void)pthread_mutex_lock(&s_control_lock);
    if (s_semaphore_creates_before_failure != UINT_MAX)
    {
        if (s_semaphore_creates_before_failure == 0)
        {
            fail = true;
            s_semaphore_creates_before_failure = UINT_MAX;
        }
        else
        {
            --s_semaphore_creates_before_failure;
        }
    }
    (void)pthread_mutex_unlock(&s_control_lock);
    return fail;
}

static SemaphoreHandle_t _host_create_semaphore(StaticSemaphore_t *storage,
        bool available, bool is_binary)
{
    if (storage == NULL || storage->initialized ||
            _host_fail_semaphore_create())
    {
        return NULL;
    }
    if (pthread_mutex_init(&storage->lock, NULL) != 0)
    {
        return NULL;
    }
    if (pthread_cond_init(&storage->changed, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&storage->lock);
        return NULL;
    }
    storage->available = available;
    storage->initialized = true;
    storage->is_binary = is_binary;
    (void)pthread_mutex_lock(&s_control_lock);
    ++s_live_semaphores;
    ++s_total_semaphore_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
    return storage;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return _host_create_semaphore(storage, true, false);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return _host_create_semaphore(storage, false, true);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t timeout_ticks)
{
    if (semaphore == NULL || !semaphore->initialized)
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    struct timespec deadline = {0};
    if (timeout_ticks != 0 && timeout_ticks != portMAX_DELAY)
    {
        deadline = _host_deadline_after_ticks(timeout_ticks);
    }
    int wait_result = 0;
    while (!semaphore->available && wait_result != ETIMEDOUT)
    {
        if (timeout_ticks == 0)
        {
            wait_result = ETIMEDOUT;
        }
        else if (timeout_ticks == portMAX_DELAY)
        {
            wait_result = pthread_cond_wait(&semaphore->changed,
                                            &semaphore->lock);
        }
        else
        {
            wait_result = pthread_cond_timedwait(&semaphore->changed,
                                                 &semaphore->lock, &deadline);
        }
    }
    if (!semaphore->available)
    {
        (void)pthread_mutex_unlock(&semaphore->lock);
        return pdFALSE;
    }
    semaphore->available = false;
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL || !semaphore->initialized)
    {
        return pdFALSE;
    }
    if (semaphore->is_binary)
    {
        (void)pthread_mutex_lock(&s_control_lock);
        if (s_block_next_binary_give)
        {
            s_block_next_binary_give = false;
            s_binary_give_blocked = true;
            s_binary_give_released = false;
            (void)pthread_cond_broadcast(&s_control_changed);
            while (!s_binary_give_released)
            {
                (void)pthread_cond_wait(&s_control_changed, &s_control_lock);
            }
            s_binary_give_blocked = false;
            s_binary_give_released = false;
            (void)pthread_cond_broadcast(&s_control_changed);
        }
        (void)pthread_mutex_unlock(&s_control_lock);
    }
    (void)pthread_mutex_lock(&semaphore->lock);
    semaphore->available = true;
    (void)pthread_cond_broadcast(&semaphore->changed);
    (void)pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL || !semaphore->initialized)
    {
        return;
    }
    (void)pthread_cond_destroy(&semaphore->changed);
    (void)pthread_mutex_destroy(&semaphore->lock);
    semaphore->available = false;
    semaphore->initialized = false;
    semaphore->is_binary = false;
    (void)pthread_mutex_lock(&s_control_lock);
    --s_live_semaphores;
    (void)pthread_cond_broadcast(&s_control_changed);
    (void)pthread_mutex_unlock(&s_control_lock);
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *buffer,
                                 StaticQueue_t *queue_storage)
{
    if (length == 0 || item_size == 0 || buffer == NULL ||
            queue_storage == NULL || queue_storage->initialized ||
            _host_consume(&s_fail_queue_creates))
    {
        return NULL;
    }
    if (pthread_mutex_init(&queue_storage->lock, NULL) != 0)
    {
        return NULL;
    }
    if (pthread_cond_init(&queue_storage->changed, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&queue_storage->lock);
        return NULL;
    }
    queue_storage->items = buffer;
    queue_storage->item_size = item_size;
    queue_storage->capacity = length;
    queue_storage->head = 0;
    queue_storage->tail = 0;
    queue_storage->count = 0;
    queue_storage->initialized = true;
    memset(buffer, 0, (size_t)length * item_size);

    (void)pthread_mutex_lock(&s_control_lock);
    bool recorded = false;
    for (size_t index = 0; index < HOST_MAX_QUEUES; ++index)
    {
        if (s_queues[index] == NULL)
        {
            s_queues[index] = queue_storage;
            recorded = true;
            break;
        }
    }
    if (!recorded)
    {
        (void)pthread_mutex_unlock(&s_control_lock);
        (void)pthread_cond_destroy(&queue_storage->changed);
        (void)pthread_mutex_destroy(&queue_storage->lock);
        queue_storage->initialized = false;
        return NULL;
    }
    ++s_live_queues;
    ++s_total_queue_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
    return queue_storage;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t timeout_ticks)
{
    if (queue == NULL || item == NULL || !queue->initialized ||
            _host_consume(&s_fail_queue_sends))
    {
        return pdFALSE;
    }
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if (timeout_ticks != 0 && timeout_ticks != portMAX_DELAY)
    {
        deadline = _host_deadline_after_ticks(timeout_ticks);
    }
    int wait_result = 0;
    while (queue->count == queue->capacity && wait_result != ETIMEDOUT)
    {
        if (timeout_ticks == 0)
        {
            wait_result = ETIMEDOUT;
        }
        else if (timeout_ticks == portMAX_DELAY)
        {
            wait_result = pthread_cond_wait(&queue->changed, &queue->lock);
        }
        else
        {
            wait_result = pthread_cond_timedwait(&queue->changed, &queue->lock,
                                                 &deadline);
        }
    }
    if (queue->count == queue->capacity)
    {
        (void)pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    memcpy(queue->items + queue->head * queue->item_size, item,
           queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    ++queue->count;
    (void)pthread_cond_broadcast(&queue->changed);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

static void _host_wait_queue_unpaused(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    while (s_queue_receive_paused)
    {
        s_queue_receive_waiting = true;
        (void)pthread_cond_broadcast(&s_control_changed);
        (void)pthread_cond_wait(&s_control_changed, &s_control_lock);
    }
    s_queue_receive_waiting = false;
    (void)pthread_mutex_unlock(&s_control_lock);
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t timeout_ticks)
{
    if (queue == NULL || item == NULL || !queue->initialized)
    {
        return pdFALSE;
    }
    _host_wait_queue_unpaused();
    (void)pthread_mutex_lock(&s_control_lock);
    bool delivery_deferred = s_queue_delivery_deferred;
    if (delivery_deferred)
    {
        s_queue_delivery_deferred_observed = true;
        (void)pthread_cond_broadcast(&s_control_changed);
    }
    (void)pthread_mutex_unlock(&s_control_lock);
    if (delivery_deferred)
    {
        const struct timespec delay =
        {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };
        (void)nanosleep(&delay, NULL);
        return pdFALSE;
    }
    const uint64_t initial_virtual_ticks = atomic_load(&s_virtual_ticks);
    (void)pthread_mutex_lock(&queue->lock);
    struct timespec deadline = {0};
    if (timeout_ticks != 0 && timeout_ticks != portMAX_DELAY)
    {
        deadline = _host_deadline_after_ticks(timeout_ticks);
    }
    int wait_result = 0;
    while (queue->count == 0 && wait_result != ETIMEDOUT &&
            atomic_load(&s_virtual_ticks) == initial_virtual_ticks)
    {
        if (timeout_ticks == 0)
        {
            wait_result = ETIMEDOUT;
        }
        else if (timeout_ticks == portMAX_DELAY)
        {
            wait_result = pthread_cond_wait(&queue->changed, &queue->lock);
        }
        else
        {
            wait_result = pthread_cond_timedwait(&queue->changed, &queue->lock,
                                                 &deadline);
        }
    }
    if (queue->count == 0)
    {
        (void)pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    memcpy(item, queue->items + queue->tail * queue->item_size,
           queue->item_size);
    memset(queue->items + queue->tail * queue->item_size, 0,
           queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    --queue->count;
    (void)pthread_cond_broadcast(&queue->changed);
    (void)pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue)
{
    if (queue == NULL || !queue->initialized)
    {
        return;
    }
    (void)pthread_cond_destroy(&queue->changed);
    (void)pthread_mutex_destroy(&queue->lock);
    queue->items = NULL;
    queue->item_size = 0;
    queue->capacity = 0;
    queue->count = 0;
    queue->initialized = false;
    (void)pthread_mutex_lock(&s_control_lock);
    for (size_t index = 0; index < HOST_MAX_QUEUES; ++index)
    {
        if (s_queues[index] == queue)
        {
            s_queues[index] = NULL;
            break;
        }
    }
    --s_live_queues;
    (void)pthread_cond_broadcast(&s_control_changed);
    (void)pthread_mutex_unlock(&s_control_lock);
}

static void *_host_task_trampoline(void *argument)
{
    TaskHandle_t task = argument;
    s_current_task = task;
    task->entry(task->context);
    vTaskDelete(NULL);
    return NULL;
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
    if (entry == NULL || task_storage == NULL || task_storage->created ||
            _host_consume(&s_fail_task_creates))
    {
        return NULL;
    }
    task_storage->entry = entry;
    task_storage->context = context;
    task_storage->created = true;
    (void)pthread_mutex_lock(&s_control_lock);
    ++s_live_tasks;
    ++s_total_task_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
    if (pthread_create(&task_storage->thread, NULL, _host_task_trampoline,
                       task_storage) != 0)
    {
        task_storage->created = false;
        (void)pthread_mutex_lock(&s_control_lock);
        --s_live_tasks;
        (void)pthread_mutex_unlock(&s_control_lock);
        return NULL;
    }
    (void)pthread_detach(task_storage->thread);
    return task_storage;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return s_current_task != NULL ? s_current_task :
           (TaskHandle_t)(void *)&s_external_task_token;
}

TickType_t xTaskGetTickCount(void)
{
    (void)pthread_once(&s_tick_once, _host_init_tick_origin);
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t nanoseconds =
        (uint64_t)(now.tv_sec - s_tick_origin.tv_sec) * UINT64_C(1000000000);
    if (now.tv_nsec >= s_tick_origin.tv_nsec)
    {
        nanoseconds += (uint64_t)(now.tv_nsec - s_tick_origin.tv_nsec);
    }
    else
    {
        nanoseconds -= UINT64_C(1000000000);
        nanoseconds += (uint64_t)(UINT64_C(1000000000) + now.tv_nsec -
                                  s_tick_origin.tv_nsec);
    }
    uint64_t real_ticks =
        (nanoseconds * configTICK_RATE_HZ) / UINT64_C(1000000000);
    return (TickType_t)(real_ticks + atomic_load(&s_virtual_ticks));
}

void vTaskDelay(TickType_t ticks)
{
    if (ticks == 0)
    {
        (void)sched_yield();
        return;
    }
    uint64_t nanoseconds =
        ((uint64_t)ticks * UINT64_C(1000000000)) / configTICK_RATE_HZ;
    if (nanoseconds > UINT64_C(1000000))
    {
        nanoseconds = UINT64_C(1000000);
    }
    struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = (long)nanoseconds,
    };
    (void)nanosleep(&delay, NULL);
}

void vTaskDelete(TaskHandle_t task)
{
    TaskHandle_t deleted = task != NULL ? task : s_current_task;
    if (deleted != NULL)
    {
        (void)pthread_mutex_lock(&s_control_lock);
        if (deleted->created)
        {
            deleted->created = false;
            --s_live_tasks;
            (void)pthread_cond_broadcast(&s_control_changed);
        }
        (void)pthread_mutex_unlock(&s_control_lock);
    }
    if (task == NULL)
    {
        s_current_task = NULL;
        pthread_exit(NULL);
    }
}

void host_freertos_reset_controls(void)
{
    host_freertos_pause_queue_receive(false);
    (void)pthread_mutex_lock(&s_control_lock);
    s_semaphore_creates_before_failure = UINT_MAX;
    s_fail_queue_creates = 0;
    s_fail_task_creates = 0;
    s_fail_queue_sends = 0;
    s_queue_receive_waiting = false;
    s_queue_delivery_deferred = false;
    s_queue_delivery_deferred_observed = false;
    s_block_next_binary_give = false;
    s_binary_give_released = true;
    (void)pthread_cond_broadcast(&s_control_changed);
    (void)pthread_mutex_unlock(&s_control_lock);
}

void host_freertos_fail_semaphore_create_after(unsigned successful_creates)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_semaphore_creates_before_failure = successful_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
}

void host_freertos_fail_next_queue_creates(unsigned count)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_fail_queue_creates = count;
    (void)pthread_mutex_unlock(&s_control_lock);
}

void host_freertos_fail_next_task_creates(unsigned count)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_fail_task_creates = count;
    (void)pthread_mutex_unlock(&s_control_lock);
}

void host_freertos_fail_next_queue_sends(unsigned count)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_fail_queue_sends = count;
    (void)pthread_mutex_unlock(&s_control_lock);
}

void host_freertos_block_next_binary_give(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_block_next_binary_give = true;
    s_binary_give_blocked = false;
    s_binary_give_released = false;
    (void)pthread_mutex_unlock(&s_control_lock);
}

bool host_freertos_wait_binary_give_blocked(uint32_t timeout_ms)
{
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_control_lock);
    int wait_result = 0;
    while (!s_binary_give_blocked && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_control_changed,
                                             &s_control_lock, &deadline);
    }
    bool blocked = s_binary_give_blocked;
    (void)pthread_mutex_unlock(&s_control_lock);
    return blocked;
}

void host_freertos_release_binary_give(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_binary_give_released = true;
    (void)pthread_cond_broadcast(&s_control_changed);
    (void)pthread_mutex_unlock(&s_control_lock);
}

void host_freertos_pause_queue_receive(bool pause)
{
    QueueHandle_t queues[HOST_MAX_QUEUES];
    (void)pthread_mutex_lock(&s_control_lock);
    s_queue_receive_paused = pause;
    if (!pause)
    {
        (void)pthread_cond_broadcast(&s_control_changed);
    }
    memcpy(queues, s_queues, sizeof(queues));
    (void)pthread_mutex_unlock(&s_control_lock);
    for (size_t index = 0; index < HOST_MAX_QUEUES; ++index)
    {
        QueueHandle_t queue = queues[index];
        if (queue != NULL && queue->initialized)
        {
            (void)pthread_mutex_lock(&queue->lock);
            (void)pthread_cond_broadcast(&queue->changed);
            (void)pthread_mutex_unlock(&queue->lock);
        }
    }
}

bool host_freertos_wait_queue_receive_paused(uint32_t timeout_ms)
{
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_control_lock);
    int wait_result = 0;
    while (!s_queue_receive_waiting && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_control_changed,
                                             &s_control_lock, &deadline);
    }
    bool waiting = s_queue_receive_waiting;
    (void)pthread_mutex_unlock(&s_control_lock);
    return waiting;
}

void host_freertos_defer_queue_delivery(bool defer)
{
    (void)pthread_mutex_lock(&s_control_lock);
    s_queue_delivery_deferred = defer;
    s_queue_delivery_deferred_observed = false;
    (void)pthread_cond_broadcast(&s_control_changed);
    (void)pthread_mutex_unlock(&s_control_lock);
}

bool host_freertos_wait_queue_delivery_deferred(uint32_t timeout_ms)
{
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_control_lock);
    int wait_result = 0;
    while (!s_queue_delivery_deferred_observed &&
            wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_control_changed,
                                             &s_control_lock, &deadline);
    }
    bool observed = s_queue_delivery_deferred_observed;
    (void)pthread_mutex_unlock(&s_control_lock);
    return observed;
}

static bool _host_freertos_queues_empty(void)
{
    QueueHandle_t queues[HOST_MAX_QUEUES];
    (void)pthread_mutex_lock(&s_control_lock);
    memcpy(queues, s_queues, sizeof(queues));
    (void)pthread_mutex_unlock(&s_control_lock);
    for (size_t index = 0; index < HOST_MAX_QUEUES; ++index)
    {
        QueueHandle_t queue = queues[index];
        if (queue == NULL || !queue->initialized)
        {
            continue;
        }
        (void)pthread_mutex_lock(&queue->lock);
        bool empty = queue->count == 0;
        (void)pthread_mutex_unlock(&queue->lock);
        if (!empty)
        {
            return false;
        }
    }
    return true;
}

bool host_freertos_wait_queues_empty(uint32_t timeout_ms)
{
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    while (!_host_freertos_queues_empty())
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
        {
            return false;
        }
        const struct timespec delay =
        {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };
        (void)nanosleep(&delay, NULL);
    }
    return true;
}

void host_freertos_advance_ticks(TickType_t ticks)
{
    QueueHandle_t queues[HOST_MAX_QUEUES];
    atomic_fetch_add(&s_virtual_ticks, ticks);
    (void)pthread_mutex_lock(&s_control_lock);
    memcpy(queues, s_queues, sizeof(queues));
    (void)pthread_mutex_unlock(&s_control_lock);
    for (size_t index = 0; index < HOST_MAX_QUEUES; ++index)
    {
        QueueHandle_t queue = queues[index];
        if (queue != NULL && queue->initialized)
        {
            (void)pthread_mutex_lock(&queue->lock);
            (void)pthread_cond_broadcast(&queue->changed);
            (void)pthread_mutex_unlock(&queue->lock);
        }
    }
}

unsigned host_freertos_live_semaphores(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    unsigned count = s_live_semaphores;
    (void)pthread_mutex_unlock(&s_control_lock);
    return count;
}

unsigned host_freertos_live_queues(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    unsigned count = s_live_queues;
    (void)pthread_mutex_unlock(&s_control_lock);
    return count;
}

unsigned host_freertos_live_tasks(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    unsigned count = s_live_tasks;
    (void)pthread_mutex_unlock(&s_control_lock);
    return count;
}

unsigned host_freertos_total_semaphore_creates(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    unsigned count = s_total_semaphore_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
    return count;
}

unsigned host_freertos_total_queue_creates(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    unsigned count = s_total_queue_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
    return count;
}

unsigned host_freertos_total_task_creates(void)
{
    (void)pthread_mutex_lock(&s_control_lock);
    unsigned count = s_total_task_creates;
    (void)pthread_mutex_unlock(&s_control_lock);
    return count;
}

bool host_freertos_wait_no_tasks(uint32_t timeout_ms)
{
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_control_lock);
    int wait_result = 0;
    while (s_live_tasks != 0 && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_control_changed,
                                             &s_control_lock, &deadline);
    }
    bool none = s_live_tasks == 0;
    (void)pthread_mutex_unlock(&s_control_lock);
    return none;
}

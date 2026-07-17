#include "host_freertos.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HOST_DYNAMIC_QUEUE_COUNT     2U
#define HOST_DYNAMIC_QUEUE_BYTES   256U
#define HOST_DYNAMIC_SEMAPHORE_COUNT 40U

typedef struct host_dynamic_queue
{
    StaticQueue_t storage;
    uint8_t bytes[HOST_DYNAMIC_QUEUE_BYTES];
} host_dynamic_queue_t;

static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static host_dynamic_queue_t s_dynamic_queues[HOST_DYNAMIC_QUEUE_COUNT];
static StaticSemaphore_t
s_dynamic_semaphores[HOST_DYNAMIC_SEMAPHORE_COUNT];

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

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    if (length == 0 || item_size == 0 ||
            (size_t)length * item_size > HOST_DYNAMIC_QUEUE_BYTES)
    {
        return NULL;
    }

    (void)pthread_mutex_lock(&s_pool_lock);
    QueueHandle_t queue = NULL;
    for (size_t index = 0; index < HOST_DYNAMIC_QUEUE_COUNT; ++index)
    {
        host_dynamic_queue_t *candidate = &s_dynamic_queues[index];
        if (!candidate->storage.initialized)
        {
            queue = xQueueCreateStatic(length, item_size, candidate->bytes,
                                       &candidate->storage);
            if (queue != NULL)
            {
                break;
            }
        }
    }
    (void)pthread_mutex_unlock(&s_pool_lock);
    return queue;
}

static SemaphoreHandle_t _host_create_dynamic_semaphore(bool mutex)
{
    (void)pthread_mutex_lock(&s_pool_lock);
    SemaphoreHandle_t semaphore = NULL;
    for (size_t index = 0; index < HOST_DYNAMIC_SEMAPHORE_COUNT; ++index)
    {
        StaticSemaphore_t *candidate = &s_dynamic_semaphores[index];
        if (!candidate->initialized)
        {
            semaphore = mutex ? xSemaphoreCreateMutexStatic(candidate) :
                        xSemaphoreCreateBinaryStatic(candidate);
            if (semaphore != NULL)
            {
                break;
            }
        }
    }
    (void)pthread_mutex_unlock(&s_pool_lock);
    return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return _host_create_dynamic_semaphore(true);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return _host_create_dynamic_semaphore(false);
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
                          (!atomic_load(&timer->paused) || stepped);
        timer->callback_running = fire;
        (void)pthread_mutex_unlock(&timer->state_lock);

        if (fire)
        {
            timer->callback(timer->arg);
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
    if (timer == NULL)
    {
        return;
    }
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

void host_timer_step(esp_timer_handle_t timer)
{
    if (timer != NULL)
    {
        atomic_fetch_add(&timer->pending_ticks, 1U);
    }
}

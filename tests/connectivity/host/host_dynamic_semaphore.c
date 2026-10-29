/**
 * @file Dynamic host semaphore pool shared by the host fixtures.
 *
 * The FreeRTOS fakes expose semaphore creation over caller-provided storage;
 * this pool provides the heap-style entry points used by production code.
 */

#include "host_freertos.h"

#include <pthread.h>
#include <stddef.h>

#define HOST_DYNAMIC_SEMAPHORE_COUNT 40U

static pthread_mutex_t s_semaphore_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static StaticSemaphore_t
s_dynamic_semaphores[HOST_DYNAMIC_SEMAPHORE_COUNT];

static SemaphoreHandle_t _host_create_dynamic_semaphore(bool mutex)
{
    (void)pthread_mutex_lock(&s_semaphore_pool_lock);
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
    (void)pthread_mutex_unlock(&s_semaphore_pool_lock);
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

/** @file time_service port for the host simulator.
 *
 * Replaces layers/middleware/components/time_service/src/time_service_port.c
 * (the port seam). The host wall clock is the time source; a virtual epoch
 * override supports CI injection (Agent sim.set_time) without requiring
 * CAP_SYS_TIME. SNTP is simulated: start/restart deliver one immediate sync
 * notification from a short-lived detached thread, mirroring the device's
 * asynchronous callback context.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "time_service_port.h"

#include "sim_time.h"

static time_service_port_sync_cb_t s_sync_cb;
static pthread_mutex_t s_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_override;
static int64_t s_override_epoch;
static atomic_bool s_sntp_enabled = true;

esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn callback, void *context)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return callback(context);
}

static int64_t _host_epoch(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec;
}

esp_err_t time_service_port_clock_set(int64_t epoch)
{
    if (epoch == 0)
    {
        /* Initial cold-boot restore without RTC data: keep the host clock. */
        return ESP_OK;
    }
    (void)pthread_mutex_lock(&s_clock_lock);
    s_override = true;
    s_override_epoch = epoch;
    (void)pthread_mutex_unlock(&s_clock_lock);
    return ESP_OK;
}

esp_err_t time_service_port_clock_get(int64_t *epoch)
{
    if (epoch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_clock_lock);
    const int64_t value = s_override ? s_override_epoch : _host_epoch();
    (void)pthread_mutex_unlock(&s_clock_lock);
    *epoch = value;
    return ESP_OK;
}

/* ------------------------------------------------------- SNTP emulation */

void esp_sntp_setoperatingmode(int mode)
{
    (void)mode;
}

void esp_sntp_setservername(int index, const char *server)
{
    (void)index;
    (void)server;
}

void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback)
{
    (void)pthread_mutex_lock(&s_clock_lock);
    s_sync_cb = callback;
    (void)pthread_mutex_unlock(&s_clock_lock);
}

static void *_sntp_fire(void *arg)
{
    struct timeval value;

    (void)arg;
    time_service_port_sync_cb_t callback;

    usleep(20000U);
    (void)pthread_mutex_lock(&s_clock_lock);
    callback = s_sync_cb;
    value.tv_sec = s_override ? s_override_epoch : _host_epoch();
    (void)pthread_mutex_unlock(&s_clock_lock);
    if (callback == NULL)
    {
        return NULL;
    }
    value.tv_usec = 0;
    callback(&value);
    return NULL;
}

static void _fire_async(void)
{
    pthread_t thread;

    if (!atomic_load(&s_sntp_enabled))
    {
        return;
    }
    if (pthread_create(&thread, NULL, _sntp_fire, NULL) == 0)
    {
        (void)pthread_detach(thread);
    }
}

void esp_sntp_init(void)
{
    _fire_async();
}

bool esp_sntp_restart(void)
{
    _fire_async();
    return true;
}

void esp_sntp_stop(void)
{
}

esp_err_t time_service_port_sntp_start(const char *server,
                                       time_service_port_sync_cb_t callback)
{
    if (server == NULL || server[0] == '\0' || callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_clock_lock);
    s_sync_cb = callback;
    (void)pthread_mutex_unlock(&s_clock_lock);
    _fire_async();
    return ESP_OK;
}

esp_err_t time_service_port_sntp_restart(void)
{
    _fire_async();
    return ESP_OK;
}

esp_err_t time_service_port_sntp_stop(void)
{
    (void)pthread_mutex_lock(&s_clock_lock);
    s_sync_cb = NULL;
    (void)pthread_mutex_unlock(&s_clock_lock);
    return ESP_OK;
}

/* ------------------------------------------------------------ CI hooks */

esp_err_t sim_time_set_epoch(int64_t epoch_seconds)
{
    (void)pthread_mutex_lock(&s_clock_lock);
    s_override = true;
    s_override_epoch = epoch_seconds;
    (void)pthread_mutex_unlock(&s_clock_lock);
    if (s_sync_cb != NULL)
    {
        _fire_async();
    }
    return ESP_OK;
}

esp_err_t sim_time_follow_host(void)
{
    (void)pthread_mutex_lock(&s_clock_lock);
    s_override = false;
    (void)pthread_mutex_unlock(&s_clock_lock);
    return ESP_OK;
}

int64_t sim_time_port_epoch(void)
{
    int64_t value;

    (void)pthread_mutex_lock(&s_clock_lock);
    value = s_override ? s_override_epoch : (int64_t) - 1;
    (void)pthread_mutex_unlock(&s_clock_lock);
    return value;
}

void sim_time_set_sntp_enabled(bool enabled)
{
    atomic_store(&s_sntp_enabled, enabled);
    if (!enabled)
    {
        (void)time_service_port_sntp_stop();
    }
}

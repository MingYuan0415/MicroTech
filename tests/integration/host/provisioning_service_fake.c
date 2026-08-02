#include "host_provisioning_service.h"

#include "event_bus.h"

#include <pthread.h>
#include <string.h>

EVENT_BUS_DEFINE_ID(PROVISIONING_SERVICE_MSG);

typedef struct host_provisioning_context
{
    pthread_mutex_t lock;
    provisioning_service_status_t status;
    unsigned open_count;
    unsigned close_count;
} host_provisioning_context_t;

static host_provisioning_context_t s_provisioning =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _host_provisioning_next_generation_locked(void)
{
    ++s_provisioning.status.generation;
    if (s_provisioning.status.generation == 0U)
    {
        ++s_provisioning.status.generation;
    }
}

void host_provisioning_service_reset(void)
{
    (void)pthread_mutex_lock(&s_provisioning.lock);
    memset(&s_provisioning.status, 0, sizeof(s_provisioning.status));
    s_provisioning.status.generation = 1U;
    s_provisioning.status.state = PROVISIONING_SERVICE_STATE_IDLE;
    s_provisioning.status.available = true;
    memcpy(s_provisioning.status.device_name, "MT-A1B2C3",
           sizeof("MT-A1B2C3"));
    s_provisioning.open_count = 0U;
    s_provisioning.close_count = 0U;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
}

unsigned host_provisioning_service_open_count(void)
{
    (void)pthread_mutex_lock(&s_provisioning.lock);
    const unsigned count = s_provisioning.open_count;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return count;
}

unsigned host_provisioning_service_close_count(void)
{
    (void)pthread_mutex_lock(&s_provisioning.lock);
    const unsigned count = s_provisioning.close_count;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return count;
}

esp_err_t host_provisioning_service_publish_status(
    const provisioning_service_status_t *status)
{
    if (status == NULL || status->generation == 0U ||
            memchr(status->device_name, '\0',
                   sizeof(status->device_name)) == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_provisioning.lock);
    s_provisioning.status = *status;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return event_bus_publish(
               PROVISIONING_SERVICE_MSG,
               PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               status, sizeof(*status), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t provisioning_service_open_window(void)
{
    provisioning_service_status_t status;
    (void)pthread_mutex_lock(&s_provisioning.lock);
    ++s_provisioning.open_count;
    s_provisioning.status.state = PROVISIONING_SERVICE_STATE_ADVERTISING;
    s_provisioning.status.last_error = ESP_OK;
    s_provisioning.status.window_remaining_ms = 600000U;
    s_provisioning.status.active = true;
    s_provisioning.status.qr_ready = true;
    _host_provisioning_next_generation_locked();
    status = s_provisioning.status;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return event_bus_publish(
               PROVISIONING_SERVICE_MSG,
               PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &status, sizeof(status), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t provisioning_service_close_window(void)
{
    provisioning_service_status_t status;
    (void)pthread_mutex_lock(&s_provisioning.lock);
    ++s_provisioning.close_count;
    s_provisioning.status.state = PROVISIONING_SERVICE_STATE_IDLE;
    s_provisioning.status.last_error = ESP_OK;
    s_provisioning.status.window_remaining_ms = 0U;
    s_provisioning.status.active = false;
    s_provisioning.status.client_connected = false;
    s_provisioning.status.wifi_operation_active = false;
    s_provisioning.status.qr_ready = false;
    _host_provisioning_next_generation_locked();
    status = s_provisioning.status;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return event_bus_publish(
               PROVISIONING_SERVICE_MSG,
               PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &status, sizeof(status), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t provisioning_service_get_status(
    provisioning_service_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_provisioning.lock);
    *status = s_provisioning.status;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return ESP_OK;
}

esp_err_t provisioning_service_copy_qr(char *output, size_t capacity,
                                       size_t *out_length)
{
    static const char qr[] =
        "{\"ver\":\"v1\",\"name\":\"MT-A1B2C3\",\"transport\":\"ble\","
        "\"security\":2,\"username\":\"microtech\",\"pop\":"
        "\"ABCDEFGHIJKLMNOPQRSTUV\",\"device_id\":\"A1B2C3\"}";
    if (output == NULL || out_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_provisioning.lock);
    const bool ready = s_provisioning.status.qr_ready;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    if (!ready)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (sizeof(qr) > capacity)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(output, qr, sizeof(qr));
    *out_length = sizeof(qr) - 1U;
    return ESP_OK;
}

bool provisioning_service_is_active(void)
{
    (void)pthread_mutex_lock(&s_provisioning.lock);
    const bool active = s_provisioning.status.active;
    (void)pthread_mutex_unlock(&s_provisioning.lock);
    return active;
}

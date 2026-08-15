#include "host_device_link_service.h"

#include "event_bus.h"

#include <pthread.h>
#include <string.h>

EVENT_BUS_DEFINE_ID(DEVICE_LINK_SERVICE_MSG);

typedef struct host_device_link_context
{
    pthread_mutex_t lock;
    device_link_service_status_t status;
    unsigned open_count;
    unsigned close_count;
    esp_err_t confirm_result;
    unsigned confirm_count;
    device_link_confirmation_token_t last_confirmation_token;
} host_device_link_context_t;

static host_device_link_context_t s_device_link =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _host_device_link_next_generation_locked(void)
{
    ++s_device_link.status.generation;
    if (s_device_link.status.generation == 0U)
    {
        ++s_device_link.status.generation;
    }
}

void host_device_link_service_reset(void)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    memset(&s_device_link.status, 0, sizeof(s_device_link.status));
    s_device_link.status.generation = 1U;
    s_device_link.status.state = DEVICE_LINK_SERVICE_STATE_ADVERTISING;
    s_device_link.status.available = true;
    s_device_link.open_count = 0U;
    s_device_link.close_count = 0U;
    s_device_link.confirm_result = ESP_OK;
    s_device_link.confirm_count = 0U;
    s_device_link.last_confirmation_token = 0U;
    (void)pthread_mutex_unlock(&s_device_link.lock);
}

unsigned host_device_link_service_open_count(void)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    const unsigned count = s_device_link.open_count;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return count;
}

unsigned host_device_link_service_close_count(void)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    const unsigned count = s_device_link.close_count;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return count;
}

void host_device_link_service_set_confirm_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    s_device_link.confirm_result = result;
    (void)pthread_mutex_unlock(&s_device_link.lock);
}

unsigned host_device_link_service_confirm_count(void)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    const unsigned count = s_device_link.confirm_count;

    (void)pthread_mutex_unlock(&s_device_link.lock);
    return count;
}

device_link_confirmation_token_t
host_device_link_service_last_confirmation_token(void)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    const device_link_confirmation_token_t token =
        s_device_link.last_confirmation_token;

    (void)pthread_mutex_unlock(&s_device_link.lock);
    return token;
}

esp_err_t host_device_link_service_publish_status(
    const device_link_service_status_t *status)
{
    if (status == NULL || status->generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_device_link.lock);
    s_device_link.status = *status;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return event_bus_publish(
               DEVICE_LINK_SERVICE_MSG,
               DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               status, sizeof(*status), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t device_link_service_open_window(void)
{
    device_link_service_status_t status;
    (void)pthread_mutex_lock(&s_device_link.lock);
    ++s_device_link.open_count;
    s_device_link.status.state = DEVICE_LINK_SERVICE_STATE_WINDOW;
    s_device_link.status.last_error = ESP_OK;
    s_device_link.status.window_remaining_ms = 600000U;
    s_device_link.status.active = true;
    s_device_link.status.qr_ready = true;
    _host_device_link_next_generation_locked();
    status = s_device_link.status;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return event_bus_publish(
               DEVICE_LINK_SERVICE_MSG,
               DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &status, sizeof(status), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t device_link_service_close_window(void)
{
    device_link_service_status_t status;
    (void)pthread_mutex_lock(&s_device_link.lock);
    ++s_device_link.close_count;
    s_device_link.status.state = DEVICE_LINK_SERVICE_STATE_ADVERTISING;
    s_device_link.status.last_error = ESP_OK;
    s_device_link.status.window_remaining_ms = 0U;
    s_device_link.status.active = false;
    s_device_link.status.client_connected = false;
    s_device_link.status.qr_ready = false;
    _host_device_link_next_generation_locked();
    status = s_device_link.status;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return event_bus_publish(
               DEVICE_LINK_SERVICE_MSG,
               DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &status, sizeof(status), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t device_link_service_confirm_binding(
    device_link_confirmation_token_t token, bool accept)
{
    (void)accept;
    (void)pthread_mutex_lock(&s_device_link.lock);
    ++s_device_link.confirm_count;
    s_device_link.last_confirmation_token = token;
    const esp_err_t result = s_device_link.confirm_result;

    (void)pthread_mutex_unlock(&s_device_link.lock);
    return result;
}

esp_err_t device_link_service_get_status(
    device_link_service_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_device_link.lock);
    *status = s_device_link.status;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return ESP_OK;
}

esp_err_t device_link_service_copy_qr(char *output, size_t capacity,
                                      size_t *out_length)
{
    static const char qr[] =
        "{\"ver\":\"link-v2\",\"name\":\"MT\","
        "\"service\":\"2c77e48c-c510-4230-8d05-63d036dc038b\","
        "\"discriminator\":\"qrvM\",\"pop\":\"AAECAwQFBgcICQoLDA0ODw\","
        "\"expires_in_ms\":120000}";
    if (output == NULL || out_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_device_link.lock);
    const bool ready = s_device_link.status.qr_ready;
    (void)pthread_mutex_unlock(&s_device_link.lock);
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

bool device_link_service_is_active(void)
{
    (void)pthread_mutex_lock(&s_device_link.lock);
    const bool active = s_device_link.status.active;
    (void)pthread_mutex_unlock(&s_device_link.lock);
    return active;
}

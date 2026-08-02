#include "host_connectivity_manager.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#ifndef EVENT_BUS_PUBLISH_FLAG_UI_LATEST
    #define EVENT_BUS_PUBLISH_FLAG_UI_LATEST (UINT32_C(1) << 1)
#endif

typedef enum
{
    HOST_CONNECTIVITY_OPERATION_NONE = 0,
    HOST_CONNECTIVITY_OPERATION_SCAN,
    HOST_CONNECTIVITY_OPERATION_CONNECT,
    HOST_CONNECTIVITY_OPERATION_DISCONNECT,
    HOST_CONNECTIVITY_OPERATION_POLICY,
} host_connectivity_operation_t;

typedef struct host_connectivity_manager_state
{
    pthread_mutex_t lock;
    bool available;
    uint64_t generation;
    connectivity_manager_operation_id_t current_operation;
    host_connectivity_operation_t operation_kind;
    connectivity_manager_status_snapshot_t status;
    connectivity_manager_scan_snapshot_t scan;
    unsigned calls[HOST_CONNECTIVITY_MANAGER_CALL_COUNT];
} host_connectivity_manager_state_t;

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);

_Static_assert(sizeof(connectivity_manager_status_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "connectivity status exceeds event bus payload capacity");
_Static_assert(sizeof(connectivity_manager_scan_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "connectivity scan exceeds event bus payload capacity");

static host_connectivity_manager_state_t s_connectivity =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .available = true,
    .generation = 2U,
    .status = {
        .generation = 1U,
        .state = CONNECTIVITY_MANAGER_STATE_IDLE,
        .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
        .available = true,
        .radio_available = true,
        .auto_connect = true,
    },
    .scan = {
        .generation = 2U,
    },
};

static uint64_t _host_connectivity_next_generation_locked(void)
{
    ++s_connectivity.generation;
    if (s_connectivity.generation == 0U)
    {
        ++s_connectivity.generation;
    }
    return s_connectivity.generation;
}

static bool _host_connectivity_credentials_valid(
    const connectivity_manager_credentials_t *credentials)
{
    if (credentials == NULL || credentials->ssid == NULL ||
            credentials->ssid_length == 0U ||
            credentials->ssid_length > CONNECTIVITY_MANAGER_SSID_MAX_BYTES ||
            memchr(credentials->ssid, '\0',
                   credentials->ssid_length) != NULL)
    {
        return false;
    }
    if (credentials->security == CONNECTIVITY_MANAGER_SECURITY_OPEN)
    {
        return credentials->password_length == 0U;
    }
    return credentials->security == CONNECTIVITY_MANAGER_SECURITY_PERSONAL &&
           credentials->password != NULL &&
           credentials->password_length >= 8U &&
           credentials->password_length <=
           CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES &&
           memchr(credentials->password, '\0',
                  credentials->password_length) == NULL;
}

static bool _host_connectivity_status_valid(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    return snapshot != NULL &&
           snapshot->state >= CONNECTIVITY_MANAGER_STATE_OFFLINE &&
           snapshot->state <= CONNECTIVITY_MANAGER_STATE_SUSPENDED &&
           snapshot->failure >= CONNECTIVITY_MANAGER_FAILURE_NONE &&
           snapshot->failure <= CONNECTIVITY_MANAGER_FAILURE_INTERNAL &&
           memchr(snapshot->ssid, '\0', sizeof(snapshot->ssid)) != NULL;
}

static bool _host_connectivity_scan_valid(
    const connectivity_manager_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL ||
            snapshot->record_count > CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS)
    {
        return false;
    }
    for (size_t index = 0U; index < snapshot->record_count; ++index)
    {
        const connectivity_manager_scan_record_t *record =
            &snapshot->records[index];
        if (record->security < CONNECTIVITY_MANAGER_SECURITY_OPEN ||
                record->security > CONNECTIVITY_MANAGER_SECURITY_UNSUPPORTED ||
                memchr(record->ssid, '\0', sizeof(record->ssid)) == NULL ||
                record->ssid[0] == '\0')
        {
            return false;
        }
    }
    return true;
}

static bool _host_connectivity_status_is_terminal_locked(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    if (snapshot->operation_id != s_connectivity.current_operation ||
            s_connectivity.current_operation == 0U)
    {
        return false;
    }
    return snapshot->operation_complete &&
           s_connectivity.operation_kind != HOST_CONNECTIVITY_OPERATION_SCAN;
}

static void _host_connectivity_complete_operation_locked(void)
{
    s_connectivity.current_operation = 0U;
    s_connectivity.operation_kind = HOST_CONNECTIVITY_OPERATION_NONE;
}

static esp_err_t _host_connectivity_admit_operation(
    host_connectivity_manager_call_t call,
    host_connectivity_operation_t operation_kind,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (out_operation_id == NULL)
    {
        (void)pthread_mutex_lock(&s_connectivity.lock);
        ++s_connectivity.calls[call];
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out_operation_id = 0U;

    (void)pthread_mutex_lock(&s_connectivity.lock);
    ++s_connectivity.calls[call];
    if (!s_connectivity.available ||
            s_connectivity.current_operation != 0U)
    {
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_connectivity.current_operation =
        _host_connectivity_next_generation_locked();
    s_connectivity.operation_kind = operation_kind;
    *out_operation_id = s_connectivity.current_operation;
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return ESP_OK;
}

void host_connectivity_manager_reset(void)
{
    (void)pthread_mutex_lock(&s_connectivity.lock);
    s_connectivity.available = true;
    s_connectivity.generation = 2U;
    s_connectivity.current_operation = 0U;
    s_connectivity.operation_kind = HOST_CONNECTIVITY_OPERATION_NONE;
    memset(&s_connectivity.status, 0, sizeof(s_connectivity.status));
    s_connectivity.status.generation = 1U;
    s_connectivity.status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    s_connectivity.status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    s_connectivity.status.available = true;
    s_connectivity.status.radio_available = true;
    s_connectivity.status.auto_connect = true;
    memset(&s_connectivity.scan, 0, sizeof(s_connectivity.scan));
    s_connectivity.scan.generation = 2U;
    memset(s_connectivity.calls, 0, sizeof(s_connectivity.calls));
    (void)pthread_mutex_unlock(&s_connectivity.lock);
}

unsigned host_connectivity_manager_call_count(
    host_connectivity_manager_call_t call)
{
    if (call < 0 || call >= HOST_CONNECTIVITY_MANAGER_CALL_COUNT)
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&s_connectivity.lock);
    const unsigned count = s_connectivity.calls[call];
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return count;
}

connectivity_manager_operation_id_t
host_connectivity_manager_current_operation(void)
{
    (void)pthread_mutex_lock(&s_connectivity.lock);
    const connectivity_manager_operation_id_t operation =
        s_connectivity.current_operation;
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return operation;
}

esp_err_t host_connectivity_manager_cache_status(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    if (!_host_connectivity_status_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_connectivity.lock);
    s_connectivity.status = *snapshot;
    s_connectivity.available = snapshot->available;
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return ESP_OK;
}

bool connectivity_manager_is_available(void)
{
    (void)pthread_mutex_lock(&s_connectivity.lock);
    ++s_connectivity.calls[HOST_CONNECTIVITY_MANAGER_CALL_IS_AVAILABLE];
    const bool available = s_connectivity.available;
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return available;
}

esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _host_connectivity_admit_operation(
               HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_SCAN,
               HOST_CONNECTIVITY_OPERATION_SCAN, out_operation_id);
}

esp_err_t connectivity_manager_request_connect(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_host_connectivity_credentials_valid(credentials))
    {
        (void)pthread_mutex_lock(&s_connectivity.lock);
        ++s_connectivity.calls[
            HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_CONNECT];
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_INVALID_ARG;
    }
    return _host_connectivity_admit_operation(
               HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_CONNECT,
               HOST_CONNECTIVITY_OPERATION_CONNECT, out_operation_id);
}

esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _host_connectivity_admit_operation(
               HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_DISCONNECT,
               HOST_CONNECTIVITY_OPERATION_DISCONNECT, out_operation_id);
}

esp_err_t connectivity_manager_request_reconnect_saved(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _host_connectivity_admit_operation(
               HOST_CONNECTIVITY_MANAGER_CALL_RECONNECT_SAVED,
               HOST_CONNECTIVITY_OPERATION_CONNECT, out_operation_id);
}

esp_err_t connectivity_manager_request_forget(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _host_connectivity_admit_operation(
               HOST_CONNECTIVITY_MANAGER_CALL_FORGET,
               HOST_CONNECTIVITY_OPERATION_DISCONNECT, out_operation_id);
}

esp_err_t connectivity_manager_set_auto_connect(
    bool enabled, connectivity_manager_operation_id_t *out_operation_id)
{
    const esp_err_t result = _host_connectivity_admit_operation(
                                 HOST_CONNECTIVITY_MANAGER_CALL_SET_AUTO_CONNECT,
                                 HOST_CONNECTIVITY_OPERATION_POLICY,
                                 out_operation_id);
    if (result == ESP_OK)
    {
        (void)pthread_mutex_lock(&s_connectivity.lock);
        s_connectivity.status.auto_connect = enabled;
        (void)pthread_mutex_unlock(&s_connectivity.lock);
    }
    return result;
}

esp_err_t connectivity_manager_cancel(
    connectivity_manager_operation_id_t operation_id)
{
    (void)pthread_mutex_lock(&s_connectivity.lock);
    ++s_connectivity.calls[HOST_CONNECTIVITY_MANAGER_CALL_CANCEL];
    if (operation_id == 0U)
    {
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (operation_id != s_connectivity.current_operation)
    {
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_NOT_FOUND;
    }
    _host_connectivity_complete_operation_locked();
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return ESP_OK;
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *snapshot)
{
    (void)pthread_mutex_lock(&s_connectivity.lock);
    ++s_connectivity.calls[HOST_CONNECTIVITY_MANAGER_CALL_GET_STATUS];
    if (snapshot == NULL)
    {
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_connectivity.status;
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return ESP_OK;
}

esp_err_t connectivity_manager_get_scan_snapshot(
    connectivity_manager_scan_snapshot_t *snapshot)
{
    (void)pthread_mutex_lock(&s_connectivity.lock);
    ++s_connectivity.calls[
        HOST_CONNECTIVITY_MANAGER_CALL_GET_SCAN_SNAPSHOT];
    if (snapshot == NULL)
    {
        (void)pthread_mutex_unlock(&s_connectivity.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_connectivity.scan;
    (void)pthread_mutex_unlock(&s_connectivity.lock);
    return ESP_OK;
}

esp_err_t host_connectivity_manager_publish_status(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    if (!_host_connectivity_status_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }
    connectivity_manager_status_snapshot_t published = *snapshot;
    (void)pthread_mutex_lock(&s_connectivity.lock);
    published.generation = _host_connectivity_next_generation_locked();
    s_connectivity.status = published;
    s_connectivity.available = published.available;
    if (_host_connectivity_status_is_terminal_locked(&published))
    {
        _host_connectivity_complete_operation_locked();
    }
    (void)pthread_mutex_unlock(&s_connectivity.lock);

    return event_bus_publish(
               CONNECTIVITY_MANAGER_MSG,
               CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &published, sizeof(published),
               published.operation_complete ? 0U :
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t host_connectivity_manager_publish_scan(
    const connectivity_manager_scan_snapshot_t *snapshot)
{
    if (!_host_connectivity_scan_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }
    connectivity_manager_scan_snapshot_t published = *snapshot;
    (void)pthread_mutex_lock(&s_connectivity.lock);
    published.generation = _host_connectivity_next_generation_locked();
    s_connectivity.scan = published;
    if (!published.running &&
            published.operation_id == s_connectivity.current_operation &&
            s_connectivity.operation_kind == HOST_CONNECTIVITY_OPERATION_SCAN)
    {
        _host_connectivity_complete_operation_locked();
    }
    (void)pthread_mutex_unlock(&s_connectivity.lock);

    return event_bus_publish(
               CONNECTIVITY_MANAGER_MSG,
               CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
               &published, sizeof(published),
               published.running ? EVENT_BUS_PUBLISH_FLAG_UI_LATEST : 0U);
}

esp_err_t host_connectivity_manager_publish_raw_scan(const void *payload,
        size_t payload_size)
{
    return event_bus_publish(
               CONNECTIVITY_MANAGER_MSG,
               CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
               payload, payload_size, EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

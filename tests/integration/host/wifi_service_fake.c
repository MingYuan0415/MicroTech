#include "host_wifi_service.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#ifndef EVENT_BUS_PUBLISH_FLAG_UI_LATEST
    #define EVENT_BUS_PUBLISH_FLAG_UI_LATEST (UINT32_C(1) << 1)
#endif

typedef enum
{
    HOST_WIFI_OPERATION_NONE = 0,
    HOST_WIFI_OPERATION_SCAN,
    HOST_WIFI_OPERATION_CONNECT,
    HOST_WIFI_OPERATION_DISCONNECT,
} host_wifi_operation_t;

typedef struct host_wifi_service_state
{
    pthread_mutex_t lock;
    bool available;
    uint64_t generation;
    wifi_service_session_id_t current_session;
    wifi_service_operation_id_t current_operation;
    host_wifi_operation_t operation_kind;
    wifi_service_status_snapshot_t status;
    wifi_service_scan_snapshot_t scan;
    unsigned calls[HOST_WIFI_SERVICE_CALL_COUNT];
} host_wifi_service_state_t;

EVENT_BUS_DEFINE_ID(WIFI_SERVICE_MSG);

_Static_assert(sizeof(wifi_service_status_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "WiFi status exceeds event bus payload capacity");
_Static_assert(sizeof(wifi_service_scan_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "WiFi scan exceeds event bus payload capacity");

static host_wifi_service_state_t s_wifi =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .available = true,
    .generation = 2,
    .status = {
        .generation = 1,
        .state = WIFI_SERVICE_STATE_IDLE,
        .last_error = ESP_OK,
        .available = true,
    },
    .scan = {
        .generation = 2,
        .state = WIFI_SERVICE_SCAN_IDLE,
        .last_error = ESP_OK,
    },
};

static void _host_wifi_secure_zero_memory(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (bytes != NULL && size > 0)
    {
        *bytes++ = 0;
        --size;
    }
}

static uint64_t _host_wifi_next_generation_locked(void)
{
    ++s_wifi.generation;
    if (s_wifi.generation == 0)
    {
        ++s_wifi.generation;
    }
    return s_wifi.generation;
}

static bool _host_wifi_credentials_valid(
    const wifi_service_credentials_t *credentials)
{
    if (credentials == NULL || credentials->ssid == NULL ||
            credentials->ssid_length == 0 ||
            credentials->ssid_length > WIFI_SERVICE_SSID_MAX_BYTES ||
            memchr(credentials->ssid, '\0',
                   credentials->ssid_length) != NULL)
    {
        return false;
    }
    if (credentials->security == WIFI_SERVICE_SECURITY_OPEN)
    {
        return credentials->password_length == 0;
    }
    return credentials->security == WIFI_SERVICE_SECURITY_PERSONAL &&
           credentials->password != NULL &&
           credentials->password_length >= 8 &&
           credentials->password_length <=
           WIFI_SERVICE_PASSWORD_MAX_BYTES &&
           memchr(credentials->password, '\0',
                  credentials->password_length) == NULL;
}

static bool _host_wifi_status_valid(
    const wifi_service_status_snapshot_t *snapshot)
{
    return snapshot != NULL &&
           snapshot->state >= WIFI_SERVICE_STATE_OFFLINE &&
           snapshot->state <= WIFI_SERVICE_STATE_SUSPENDED &&
           ((snapshot->session_id == 0) ==
            (snapshot->operation_id == 0)) &&
           memchr(snapshot->ssid, '\0', sizeof(snapshot->ssid)) != NULL;
}

static bool _host_wifi_scan_valid(
    const wifi_service_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->state < WIFI_SERVICE_SCAN_IDLE ||
            snapshot->state > WIFI_SERVICE_SCAN_FAILED ||
            snapshot->record_count > WIFI_SERVICE_MAX_SCAN_RECORDS)
    {
        return false;
    }
    for (size_t index = 0; index < WIFI_SERVICE_MAX_SCAN_RECORDS; ++index)
    {
        const wifi_service_scan_record_t *record = &snapshot->records[index];
        if (record->security < WIFI_SERVICE_SECURITY_OPEN ||
                record->security > WIFI_SERVICE_SECURITY_UNSUPPORTED ||
                memchr(record->ssid, '\0', sizeof(record->ssid)) == NULL ||
                (index < snapshot->record_count && record->ssid[0] == '\0'))
        {
            return false;
        }
    }
    return true;
}

static bool _host_wifi_status_is_terminal_locked(
    const wifi_service_status_snapshot_t *snapshot)
{
    if (snapshot->session_id != s_wifi.current_session ||
            snapshot->operation_id != s_wifi.current_operation ||
            s_wifi.current_operation == 0)
    {
        return false;
    }
    if (s_wifi.operation_kind == HOST_WIFI_OPERATION_CONNECT)
    {
        return snapshot->state == WIFI_SERVICE_STATE_IP_READY ||
               snapshot->state == WIFI_SERVICE_STATE_IDLE ||
               snapshot->state == WIFI_SERVICE_STATE_OFFLINE;
    }
    if (s_wifi.operation_kind == HOST_WIFI_OPERATION_DISCONNECT)
    {
        return snapshot->state == WIFI_SERVICE_STATE_IDLE ||
               snapshot->state == WIFI_SERVICE_STATE_OFFLINE ||
               snapshot->state == WIFI_SERVICE_STATE_IP_READY;
    }
    return false;
}

static bool _host_wifi_scan_is_terminal_locked(
    const wifi_service_scan_snapshot_t *snapshot)
{
    return s_wifi.operation_kind == HOST_WIFI_OPERATION_SCAN &&
           s_wifi.current_operation != 0 &&
           snapshot->session_id == s_wifi.current_session &&
           snapshot->operation_id == s_wifi.current_operation &&
           (snapshot->state == WIFI_SERVICE_SCAN_RESULTS ||
            snapshot->state == WIFI_SERVICE_SCAN_CANCELED ||
            snapshot->state == WIFI_SERVICE_SCAN_FAILED);
}

static void _host_wifi_complete_operation_locked(void)
{
    s_wifi.current_operation = 0;
    s_wifi.operation_kind = HOST_WIFI_OPERATION_NONE;
}

static bool _host_wifi_scan_allowed_locked(void)
{
    return s_wifi.status.state == WIFI_SERVICE_STATE_IDLE ||
           s_wifi.status.state == WIFI_SERVICE_STATE_IP_READY;
}

static esp_err_t _host_wifi_admit_operation(
    host_wifi_service_call_t call, host_wifi_operation_t operation_kind,
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id)
{
    if (session_id == 0 || out_operation_id == NULL)
    {
        (void)pthread_mutex_lock(&s_wifi.lock);
        ++s_wifi.calls[call];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out_operation_id = 0;

    (void)pthread_mutex_lock(&s_wifi.lock);
    ++s_wifi.calls[call];
    if (!s_wifi.available || s_wifi.current_session != session_id ||
            s_wifi.current_operation != 0 ||
            (operation_kind == HOST_WIFI_OPERATION_SCAN &&
             !_host_wifi_scan_allowed_locked()))
    {
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi.current_operation = _host_wifi_next_generation_locked();
    s_wifi.operation_kind = operation_kind;
    *out_operation_id = s_wifi.current_operation;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

void host_wifi_service_reset(void)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    s_wifi.available = true;
    s_wifi.generation = 2;
    s_wifi.current_session = 0;
    s_wifi.current_operation = 0;
    s_wifi.operation_kind = HOST_WIFI_OPERATION_NONE;
    memset(&s_wifi.status, 0, sizeof(s_wifi.status));
    s_wifi.status.generation = 1;
    s_wifi.status.state = WIFI_SERVICE_STATE_IDLE;
    s_wifi.status.last_error = ESP_OK;
    s_wifi.status.available = true;
    memset(&s_wifi.scan, 0, sizeof(s_wifi.scan));
    s_wifi.scan.generation = 2;
    s_wifi.scan.state = WIFI_SERVICE_SCAN_IDLE;
    s_wifi.scan.last_error = ESP_OK;
    memset(s_wifi.calls, 0, sizeof(s_wifi.calls));
    (void)pthread_mutex_unlock(&s_wifi.lock);
}

unsigned host_wifi_service_call_count(host_wifi_service_call_t call)
{
    if (call < 0 || call >= HOST_WIFI_SERVICE_CALL_COUNT)
    {
        return 0;
    }
    (void)pthread_mutex_lock(&s_wifi.lock);
    unsigned count = s_wifi.calls[call];
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return count;
}

wifi_service_session_id_t host_wifi_service_current_session(void)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    wifi_service_session_id_t session = s_wifi.current_session;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return session;
}

wifi_service_operation_id_t host_wifi_service_current_operation(void)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    wifi_service_operation_id_t operation = s_wifi.current_operation;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return operation;
}

esp_err_t host_wifi_service_cache_status(
    const wifi_service_status_snapshot_t *snapshot)
{
    if (!_host_wifi_status_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_wifi.lock);
    s_wifi.status = *snapshot;
    s_wifi.available = snapshot->available;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

bool wifi_service_is_available(void)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_IS_AVAILABLE];
    bool available = s_wifi.available;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return available;
}

esp_err_t wifi_service_session_open(
    wifi_service_session_id_t *out_session_id)
{
    if (out_session_id == NULL)
    {
        (void)pthread_mutex_lock(&s_wifi.lock);
        ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_SESSION_OPEN];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out_session_id = 0;

    (void)pthread_mutex_lock(&s_wifi.lock);
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_SESSION_OPEN];
    if (!s_wifi.available)
    {
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi.current_session = _host_wifi_next_generation_locked();
    _host_wifi_complete_operation_locked();
    *out_session_id = s_wifi.current_session;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

esp_err_t wifi_service_session_close(wifi_service_session_id_t session_id)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    if (session_id == 0)
    {
        ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_SESSION_CLOSE];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_SESSION_CLOSE];
    if (session_id != s_wifi.current_session)
    {
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_NOT_FOUND;
    }

    s_wifi.current_session = 0;
    _host_wifi_complete_operation_locked();
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

esp_err_t wifi_service_request_scan(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id)
{
    return _host_wifi_admit_operation(HOST_WIFI_SERVICE_CALL_REQUEST_SCAN,
                                      HOST_WIFI_OPERATION_SCAN, session_id,
                                      out_operation_id);
}

esp_err_t wifi_service_request_disconnect(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id)
{
    return _host_wifi_admit_operation(
               HOST_WIFI_SERVICE_CALL_REQUEST_DISCONNECT,
               HOST_WIFI_OPERATION_DISCONNECT, session_id,
               out_operation_id);
}

esp_err_t wifi_service_request_connect(
    wifi_service_session_id_t session_id,
    const wifi_service_credentials_t *credentials,
    wifi_service_operation_id_t *out_operation_id)
{
    if (session_id == 0 || out_operation_id == NULL ||
            !_host_wifi_credentials_valid(credentials))
    {
        (void)pthread_mutex_lock(&s_wifi.lock);
        ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_REQUEST_CONNECT];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out_operation_id = 0;

    (void)pthread_mutex_lock(&s_wifi.lock);
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_REQUEST_CONNECT];
    if (!s_wifi.available || s_wifi.current_session != session_id ||
            s_wifi.current_operation != 0)
    {
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi.current_operation = _host_wifi_next_generation_locked();
    s_wifi.operation_kind = HOST_WIFI_OPERATION_CONNECT;
    *out_operation_id = s_wifi.current_operation;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

esp_err_t wifi_service_cancel(wifi_service_session_id_t session_id,
                              wifi_service_operation_id_t operation_id)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    if (session_id == 0 || operation_id == 0)
    {
        ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_CANCEL];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_CANCEL];
    if (session_id != s_wifi.current_session ||
            operation_id != s_wifi.current_operation)
    {
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_NOT_FOUND;
    }

    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

esp_err_t wifi_service_get_status(
    wifi_service_status_snapshot_t *snapshot)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    if (snapshot == NULL)
    {
        ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_GET_STATUS];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_GET_STATUS];
    *snapshot = s_wifi.status;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

esp_err_t wifi_service_get_scan_snapshot(
    wifi_service_scan_snapshot_t *snapshot)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    if (snapshot == NULL)
    {
        ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_GET_SCAN_SNAPSHOT];
        (void)pthread_mutex_unlock(&s_wifi.lock);
        return ESP_ERR_INVALID_ARG;
    }
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_GET_SCAN_SNAPSHOT];
    *snapshot = s_wifi.scan;
    (void)pthread_mutex_unlock(&s_wifi.lock);
    return ESP_OK;
}

void wifi_service_secure_zero(void *memory, size_t size)
{
    (void)pthread_mutex_lock(&s_wifi.lock);
    ++s_wifi.calls[HOST_WIFI_SERVICE_CALL_SECURE_ZERO];
    (void)pthread_mutex_unlock(&s_wifi.lock);
    _host_wifi_secure_zero_memory(memory, size);
}

esp_err_t host_wifi_service_publish_status(
    const wifi_service_status_snapshot_t *snapshot)
{
    if (!_host_wifi_status_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_service_status_snapshot_t published = *snapshot;
    (void)pthread_mutex_lock(&s_wifi.lock);
    published.generation = _host_wifi_next_generation_locked();
    s_wifi.status = published;
    s_wifi.available = published.available;
    if (_host_wifi_status_is_terminal_locked(&published))
    {
        _host_wifi_complete_operation_locked();
    }
    (void)pthread_mutex_unlock(&s_wifi.lock);

    return event_bus_publish(
               WIFI_SERVICE_MSG,
               WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &published, sizeof(published),
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t host_wifi_service_publish_scan(
    const wifi_service_scan_snapshot_t *snapshot)
{
    if (!_host_wifi_scan_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_service_scan_snapshot_t published = *snapshot;
    (void)pthread_mutex_lock(&s_wifi.lock);
    published.generation = _host_wifi_next_generation_locked();
    s_wifi.scan = published;
    if (_host_wifi_scan_is_terminal_locked(&published))
    {
        _host_wifi_complete_operation_locked();
    }
    (void)pthread_mutex_unlock(&s_wifi.lock);

    return event_bus_publish(
               WIFI_SERVICE_MSG,
               WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
               &published, sizeof(published),
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

esp_err_t host_wifi_service_publish_raw_scan(const void *payload,
        size_t payload_size)
{
    return event_bus_publish(
               WIFI_SERVICE_MSG,
               WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
               payload, payload_size, EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

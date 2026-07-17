#include "host_wifi_port.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct host_wifi_port_operation_state
{
    unsigned calls;
    unsigned failures;
    esp_err_t failure_result;
    esp_err_t default_result;
    bool gate_enabled;
    bool gate_entered;
    bool gate_released;
} host_wifi_port_operation_state_t;

typedef struct host_wifi_port_state
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    host_wifi_port_operation_state_t operations[HOST_WIFI_PORT_OPERATION_COUNT];
    pthread_t worker_thread;
    bool worker_thread_valid;
    bool thread_violation;
    wifi_service_port_state_t port_state;
    bool leave_dirty_after_next_deinit;
    bool scan_owned;
    bool scan_ownership_violation;
    bool leave_scan_owned_after_next_finish;
    bool preserve_scan_on_next_stop;
    bool leave_partial_after_next_start_failure;
    uint64_t epoch;
    uint8_t scan_id;
    wifi_service_port_credentials_t current_credentials;
    wifi_service_port_credentials_t last_credentials;
    bool last_credentials_valid;
    wifi_service_port_scan_record_t
    scan_records[WIFI_SERVICE_MAX_SCAN_RECORDS];
    size_t scan_count;
    bool scan_truncated;
} host_wifi_port_state_t;

static host_wifi_port_state_t s_port =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .port_state = WIFI_SERVICE_PORT_STATE_CLEAN,
};

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

static void _host_wifi_port_check_thread_locked(bool initializing)
{
    pthread_t current = pthread_self();
    if (initializing)
    {
        s_port.worker_thread = current;
        s_port.worker_thread_valid = true;
    }
    else if (!s_port.worker_thread_valid ||
             !pthread_equal(s_port.worker_thread, current))
    {
        s_port.thread_violation = true;
    }
}

static esp_err_t _host_wifi_port_begin_locked(
    host_wifi_port_operation_t operation)
{
    host_wifi_port_operation_state_t *state = &s_port.operations[operation];
    ++state->calls;
    _host_wifi_port_check_thread_locked(operation == HOST_WIFI_PORT_INIT);
    state->gate_entered = true;
    (void)pthread_cond_broadcast(&s_port.changed);
    while (state->gate_enabled && !state->gate_released)
    {
        (void)pthread_cond_wait(&s_port.changed, &s_port.lock);
    }
    state->gate_enabled = false;
    esp_err_t result = state->default_result;
    if (state->failures > 0)
    {
        --state->failures;
        result = state->failure_result;
    }
    return result;
}

void host_wifi_port_reset(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    memset(s_port.operations, 0, sizeof(s_port.operations));
    s_port.worker_thread_valid = false;
    s_port.thread_violation = false;
    s_port.port_state = WIFI_SERVICE_PORT_STATE_CLEAN;
    s_port.leave_dirty_after_next_deinit = false;
    s_port.scan_owned = false;
    s_port.scan_ownership_violation = false;
    s_port.leave_scan_owned_after_next_finish = false;
    s_port.preserve_scan_on_next_stop = false;
    s_port.leave_partial_after_next_start_failure = false;
    s_port.epoch = 0;
    s_port.scan_id = 0;
    memset(&s_port.current_credentials, 0,
           sizeof(s_port.current_credentials));
    memset(&s_port.last_credentials, 0, sizeof(s_port.last_credentials));
    s_port.last_credentials_valid = false;
    memset(s_port.scan_records, 0, sizeof(s_port.scan_records));
    s_port.scan_count = 0;
    s_port.scan_truncated = false;
    (void)pthread_cond_broadcast(&s_port.changed);
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_fail_next(host_wifi_port_operation_t operation,
                              unsigned count, esp_err_t result)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.operations[operation].failures = count;
    s_port.operations[operation].failure_result = result;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_set_default_result(host_wifi_port_operation_t operation,
                                       esp_err_t result)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.operations[operation].default_result = result;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_leave_dirty_after_next_deinit(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.leave_dirty_after_next_deinit = true;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_leave_scan_owned_after_next_finish(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.leave_scan_owned_after_next_finish = true;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_preserve_scan_on_next_stop(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.preserve_scan_on_next_stop = true;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_leave_partial_after_next_start_failure(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.leave_partial_after_next_start_failure = true;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_wifi_port_gate(host_wifi_port_operation_t operation, bool enabled)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    host_wifi_port_operation_state_t *state = &s_port.operations[operation];
    state->gate_enabled = enabled;
    state->gate_entered = false;
    state->gate_released = !enabled;
    if (!enabled)
    {
        (void)pthread_cond_broadcast(&s_port.changed);
    }
    (void)pthread_mutex_unlock(&s_port.lock);
}

bool host_wifi_port_wait_gate(host_wifi_port_operation_t operation,
                              uint32_t timeout_ms)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return false;
    }
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_port.lock);
    int wait_result = 0;
    while (!s_port.operations[operation].gate_entered &&
            wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_port.changed, &s_port.lock,
                                             &deadline);
    }
    bool entered = s_port.operations[operation].gate_entered;
    (void)pthread_mutex_unlock(&s_port.lock);
    return entered;
}

void host_wifi_port_release_gate(host_wifi_port_operation_t operation)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.operations[operation].gate_released = true;
    (void)pthread_cond_broadcast(&s_port.changed);
    (void)pthread_mutex_unlock(&s_port.lock);
}

unsigned host_wifi_port_call_count(host_wifi_port_operation_t operation)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return 0;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    unsigned calls = s_port.operations[operation].calls;
    (void)pthread_mutex_unlock(&s_port.lock);
    return calls;
}

bool host_wifi_port_wait_calls(host_wifi_port_operation_t operation,
                               unsigned expected, uint32_t timeout_ms)
{
    if (operation >= HOST_WIFI_PORT_OPERATION_COUNT)
    {
        return false;
    }
    const struct timespec deadline = _host_deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_port.lock);
    int wait_result = 0;
    while (s_port.operations[operation].calls < expected &&
            wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_port.changed, &s_port.lock,
                                             &deadline);
    }
    bool reached = s_port.operations[operation].calls >= expected;
    (void)pthread_mutex_unlock(&s_port.lock);
    return reached;
}

bool host_wifi_port_is_clean_snapshot(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    bool clean = s_port.port_state == WIFI_SERVICE_PORT_STATE_CLEAN;
    (void)pthread_mutex_unlock(&s_port.lock);
    return clean;
}

wifi_service_port_state_t host_wifi_port_state(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    wifi_service_port_state_t state = s_port.port_state;
    (void)pthread_mutex_unlock(&s_port.lock);
    return state;
}

bool host_wifi_port_thread_violation(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    bool violation = s_port.thread_violation;
    (void)pthread_mutex_unlock(&s_port.lock);
    return violation;
}

bool host_wifi_port_scan_ownership_violation(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    bool violation = s_port.scan_ownership_violation;
    (void)pthread_mutex_unlock(&s_port.lock);
    return violation;
}

bool host_wifi_port_scan_owned(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    bool owned = s_port.scan_owned;
    (void)pthread_mutex_unlock(&s_port.lock);
    return owned;
}

uint64_t host_wifi_port_epoch(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    uint64_t epoch = s_port.epoch;
    (void)pthread_mutex_unlock(&s_port.lock);
    return epoch;
}

uint8_t host_wifi_port_scan_id(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    uint8_t scan_id = s_port.scan_id;
    (void)pthread_mutex_unlock(&s_port.lock);
    return scan_id;
}

void host_wifi_port_set_scan_records(
    const wifi_service_port_scan_record_t *records, size_t count,
    bool truncated)
{
    if (count > WIFI_SERVICE_MAX_SCAN_RECORDS)
    {
        count = WIFI_SERVICE_MAX_SCAN_RECORDS;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    memset(s_port.scan_records, 0, sizeof(s_port.scan_records));
    if (records != NULL && count > 0)
    {
        memcpy(s_port.scan_records, records, count * sizeof(records[0]));
    }
    s_port.scan_count = count;
    s_port.scan_truncated = truncated;
    (void)pthread_mutex_unlock(&s_port.lock);
}

bool host_wifi_port_last_credentials(
    wifi_service_port_credentials_t *credentials)
{
    (void)pthread_mutex_lock(&s_port.lock);
    bool valid = s_port.last_credentials_valid;
    if (valid && credentials != NULL)
    {
        *credentials = s_port.last_credentials;
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return valid;
}

esp_err_t wifi_service_port_init(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_INIT);
    s_port.port_state = WIFI_SERVICE_PORT_STATE_PARTIAL;
    if (result == ESP_OK)
    {
        s_port.port_state = WIFI_SERVICE_PORT_STATE_STARTED;
        ++s_port.epoch;
        if (s_port.epoch == 0)
        {
            ++s_port.epoch;
        }
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_deinit(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_DEINIT);
    if (result == ESP_OK && !s_port.leave_dirty_after_next_deinit)
    {
        s_port.port_state = WIFI_SERVICE_PORT_STATE_CLEAN;
        s_port.scan_owned = false;
        memset(&s_port.current_credentials, 0,
               sizeof(s_port.current_credentials));
    }
    s_port.leave_dirty_after_next_deinit = false;
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

bool wifi_service_port_is_clean(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    _host_wifi_port_check_thread_locked(false);
    bool clean = s_port.port_state == WIFI_SERVICE_PORT_STATE_CLEAN;
    (void)pthread_mutex_unlock(&s_port.lock);
    return clean;
}

wifi_service_port_state_t wifi_service_port_get_state(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    (void)_host_wifi_port_begin_locked(HOST_WIFI_PORT_GET_STATE);
    wifi_service_port_state_t state = s_port.port_state;
    (void)pthread_mutex_unlock(&s_port.lock);
    return state;
}

bool wifi_service_port_scan_is_owned(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    (void)_host_wifi_port_begin_locked(HOST_WIFI_PORT_SCAN_IS_OWNED);
    bool owned = s_port.scan_owned;
    (void)pthread_mutex_unlock(&s_port.lock);
    return owned;
}

uint64_t wifi_service_port_get_epoch(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    (void)_host_wifi_port_begin_locked(HOST_WIFI_PORT_GET_EPOCH);
    uint64_t epoch = s_port.epoch;
    (void)pthread_mutex_unlock(&s_port.lock);
    return epoch;
}

esp_err_t wifi_service_port_start(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_START);
    if (result == ESP_OK)
    {
        s_port.port_state = WIFI_SERVICE_PORT_STATE_STARTED;
        ++s_port.epoch;
        if (s_port.epoch == 0)
        {
            ++s_port.epoch;
        }
    }
    else if (s_port.leave_partial_after_next_start_failure)
    {
        s_port.port_state = WIFI_SERVICE_PORT_STATE_PARTIAL;
    }
    s_port.leave_partial_after_next_start_failure = false;
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_stop(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_STOP);
    if (result == ESP_OK)
    {
        s_port.port_state = WIFI_SERVICE_PORT_STATE_STOPPED;
        if (!s_port.preserve_scan_on_next_stop)
        {
            s_port.scan_owned = false;
        }
        s_port.preserve_scan_on_next_stop = false;
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_scan_start(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_SCAN_START);
    if (result == ESP_OK)
    {
        s_port.scan_owned = true;
        ++s_port.scan_id;
        if (s_port.scan_id == 0)
        {
            ++s_port.scan_id;
        }
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_scan_finish(
    wifi_service_port_scan_record_t *records, size_t capacity,
    size_t *out_count, bool *out_truncated)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_SCAN_FINISH);
    if (!s_port.scan_owned)
    {
        s_port.scan_ownership_violation = true;
    }
    if (!s_port.leave_scan_owned_after_next_finish)
    {
        s_port.scan_owned = false;
    }
    s_port.leave_scan_owned_after_next_finish = false;
    if (result == ESP_OK && records != NULL && out_count != NULL &&
            out_truncated != NULL)
    {
        size_t count = s_port.scan_count < capacity ? s_port.scan_count :
                       capacity;
        memcpy(records, s_port.scan_records, count * sizeof(records[0]));
        *out_count = count;
        *out_truncated = s_port.scan_truncated || s_port.scan_count > capacity;
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_scan_abort(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_SCAN_ABORT);
    if (!s_port.scan_owned)
    {
        s_port.scan_ownership_violation = true;
    }
    if (result == ESP_OK)
    {
        s_port.scan_owned = false;
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_set_credentials(
    const wifi_service_port_credentials_t *credentials)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(
                           HOST_WIFI_PORT_SET_CREDENTIALS);
    if (credentials != NULL)
    {
        s_port.last_credentials = *credentials;
        s_port.last_credentials_valid = true;
        if (result == ESP_OK)
        {
            s_port.current_credentials = *credentials;
        }
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_clear_credentials(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(
                           HOST_WIFI_PORT_CLEAR_CREDENTIALS);
    if (result == ESP_OK)
    {
        memset(&s_port.current_credentials, 0,
               sizeof(s_port.current_credentials));
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_connect(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_CONNECT);
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t wifi_service_port_disconnect(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    esp_err_t result = _host_wifi_port_begin_locked(HOST_WIFI_PORT_DISCONNECT);
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

#include "app_manager_mailbox.h"
#include "connectivity_manager.h"
#include "event_bus.h"
#include "freertos/task.h"
#include "host_freertos.h"
#include "host_nv_storage.h"
#include "host_wifi_port.h"
#include "setup_wifi_adapter.h"
#include "wifi_service.h"
#include "wifi_service_port.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_TIMEOUT_MS 5000U
/* Storage width for one persisted profile record. The v4 saved-network
 * record reserves five entry slots, so tests must not assume the
 * previous 168-byte single-entry layout. */
#define TEST_PROFILE_BYTES 1024U

/* v4 saved-network record layout: fixed header, then five entry slots. */
enum
{
    PROFILE_VERSION_OFFSET = 4U,
    PROFILE_COUNT_OFFSET = 6U,
    PROFILE_ACTIVE_OFFSET = 7U,
    PROFILE_AUTO_CONNECT_OFFSET = 8U,
    PROFILE_RESERVED_OFFSET = 9U,
    PROFILE_REVISION_OFFSET = 16U,
    PROFILE_SYNC_ID_OFFSET = 24U,
    PROFILE_SYNC_DIGEST_OFFSET = 32U,
    PROFILE_SYNC_VALID_OFFSET = 64U,
    PROFILE_SYNC_RESERVED_OFFSET = 65U,
    PROFILE_ENTRIES_OFFSET = 72U,
    PROFILE_ENTRY_BYTES = 103U,
    PROFILE_ENTRY_RESERVED_OFFSET = PROFILE_ENTRIES_OFFSET + 3U,
    PROFILE_ENTRY_SSID_OFFSET = PROFILE_ENTRIES_OFFSET + 4U,
    PROFILE_ENTRY_PASSWORD_OFFSET = PROFILE_ENTRIES_OFFSET + 36U,
    PROFILE_ENTRY_TRAILING_OFFSET = PROFILE_ENTRIES_OFFSET + 101U,
    PROFILE_RECORD_BYTES = 592U,
};
#define UI_TIMEOUT_MS   2000U
#define STATUS_STATE_COUNT \
    ((size_t)CONNECTIVITY_MANAGER_STATE_SUSPENDED + 1U)
#define TERMINAL_RECORD_CAPACITY 64U

#define CHECK(expression)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(expression))                                                   \
        {                                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                 \
                    __FILE__, __LINE__, #expression);                        \
            return false;                                                    \
        }                                                                    \
    } while (0)

typedef struct observer
{
    pthread_mutex_t lock;
    bool identity_violation;
    unsigned status_count;
    unsigned status_states[STATUS_STATE_COUNT];
    setup_wifi_status_scope_t last_scope;
    setup_wifi_operation_kind_t last_operation_kind;
    connectivity_manager_status_snapshot_t status;
} observer_t;

typedef struct open_args
{
    setup_wifi_adapter_t *adapter;
    observer_t *observer;
} open_args_t;

typedef struct terminal_record
{
    connectivity_manager_operation_id_t operation_id;
    uint64_t generation;
    esp_err_t result;
    bool scan;
    bool operation_canceled;
    bool operation_conflict;
    connectivity_manager_failure_t failure;
} terminal_record_t;

typedef struct terminal_observer
{
    pthread_mutex_t lock;
    size_t count;
    terminal_record_t records[TERMINAL_RECORD_CAPACITY];
} terminal_observer_t;

static const connectivity_manager_config_t s_manager_config =
{
    .task_priority = 4U,
    .wifi_task_priority = 4U,
};

static TaskHandle_t s_ui_worker;
static app_manager_ui_dispatch_fn s_real_ui_dispatch;
static atomic_bool s_block_ui_dispatch = false;
static void _sleep_one_ms(void);
static bool _wait_long_retry(connectivity_manager_failure_t failure,
                             connectivity_manager_status_snapshot_t *output);
static terminal_observer_t s_terminals =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _terminal_observer_reset(void)
{
    (void)pthread_mutex_lock(&s_terminals.lock);
    s_terminals.count = 0U;
    memset(s_terminals.records, 0, sizeof(s_terminals.records));
    (void)pthread_mutex_unlock(&s_terminals.lock);
}

static void _terminal_observer_record(
    connectivity_manager_operation_id_t operation_id, uint64_t generation,
    esp_err_t result, bool scan, bool operation_canceled,
    bool operation_conflict, connectivity_manager_failure_t failure)
{
    if (operation_id == 0U)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_terminals.lock);
    if (s_terminals.count < TERMINAL_RECORD_CAPACITY)
    {
        terminal_record_t *record = &s_terminals.records[s_terminals.count++];
        record->operation_id = operation_id;
        record->generation = generation;
        record->result = result;
        record->scan = scan;
        record->operation_canceled = operation_canceled;
        record->operation_conflict = operation_conflict;
        record->failure = failure;
    }
    (void)pthread_mutex_unlock(&s_terminals.lock);
}

static void _terminal_status_event(
    event_bus_msg_id_t msg_id, uint32_t sub_type,
    const void *payload, size_t payload_size, void *user_data)
{
    (void)user_data;
    if (msg_id == CONNECTIVITY_MANAGER_MSG &&
            sub_type ==
            CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT &&
            payload != NULL &&
            payload_size == sizeof(connectivity_manager_status_snapshot_t))
    {
        const connectivity_manager_status_snapshot_t *snapshot = payload;
        if (snapshot->operation_complete)
        {
            _terminal_observer_record(snapshot->operation_id,
                                      snapshot->generation,
                                      snapshot->last_error, false,
                                      snapshot->operation_canceled,
                                      snapshot->operation_conflict,
                                      snapshot->failure);
        }
    }
}

static void _terminal_scan_event(
    event_bus_msg_id_t msg_id, uint32_t sub_type,
    const void *payload, size_t payload_size, void *user_data)
{
    (void)user_data;
    if (msg_id == CONNECTIVITY_MANAGER_MSG &&
            sub_type == CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT &&
            payload != NULL &&
            payload_size == sizeof(connectivity_manager_scan_snapshot_t))
    {
        const connectivity_manager_scan_snapshot_t *snapshot = payload;
        if (!snapshot->running)
        {
            _terminal_observer_record(snapshot->operation_id,
                                      snapshot->generation,
                                      snapshot->last_error, true,
                                      snapshot->operation_canceled, false,
                                      CONNECTIVITY_MANAGER_FAILURE_NONE);
        }
    }
}

static void _scan_ui_event(
    event_bus_msg_id_t message_id, uint32_t subtype,
    const void *payload, size_t payload_size, void *user_data)
{
    (void)message_id;
    (void)subtype;
    (void)payload;
    (void)payload_size;
    (void)user_data;
}

static uint64_t _terminal_observer_generation(
    connectivity_manager_operation_id_t operation_id, bool scan)
{
    uint64_t generation = 0U;
    (void)pthread_mutex_lock(&s_terminals.lock);
    for (size_t index = 0U; index < s_terminals.count; ++index)
    {
        const terminal_record_t *record = &s_terminals.records[index];
        if (record->operation_id == operation_id && record->scan == scan)
        {
            generation = record->generation;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_terminals.lock);
    return generation;
}

static esp_err_t _test_ui_dispatch(void (*callback)(void *), void *argument)
{
    if (atomic_load_explicit(&s_block_ui_dispatch, memory_order_acquire))
    {
        return ESP_FAIL;
    }
    return s_real_ui_dispatch(callback, argument);
}

static unsigned _terminal_observer_count(
    connectivity_manager_operation_id_t operation_id, esp_err_t result,
    bool scan)
{
    unsigned count = 0U;
    (void)pthread_mutex_lock(&s_terminals.lock);
    for (size_t index = 0U; index < s_terminals.count; ++index)
    {
        const terminal_record_t *record = &s_terminals.records[index];
        if (record->operation_id == operation_id &&
                record->result == result && record->scan == scan)
        {
            ++count;
        }
    }
    (void)pthread_mutex_unlock(&s_terminals.lock);
    return count;
}

static bool _terminal_observer_copy(
    connectivity_manager_operation_id_t operation_id, bool scan,
    terminal_record_t *output)
{
    bool found = false;
    (void)pthread_mutex_lock(&s_terminals.lock);
    for (size_t index = 0U; index < s_terminals.count; ++index)
    {
        const terminal_record_t *record = &s_terminals.records[index];
        if (record->operation_id == operation_id && record->scan == scan)
        {
            if (output != NULL)
            {
                *output = *record;
            }
            found = true;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_terminals.lock);
    return found;
}

static bool _wait_terminal(
    connectivity_manager_operation_id_t operation_id, esp_err_t result,
    bool scan)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (_terminal_observer_count(operation_id, result, scan) > 0U)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static void _sleep_one_ms(void)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static void _observer_init(observer_t *observer)
{
    memset(observer, 0, sizeof(*observer));
    (void)pthread_mutex_init(&observer->lock, NULL);
}

static void _observer_deinit(observer_t *observer)
{
    (void)pthread_mutex_destroy(&observer->lock);
}

static void _observer_status(
    const connectivity_manager_status_snapshot_t *snapshot,
    setup_wifi_status_scope_t scope,
    setup_wifi_operation_kind_t operation_kind,
    void *user_data)
{
    observer_t *observer = user_data;
    (void)pthread_mutex_lock(&observer->lock);
    if (xTaskGetCurrentTaskHandle() != s_ui_worker ||
            !app_manager_mailbox_is_worker())
    {
        observer->identity_violation = true;
    }
    ++observer->status_count;
    if ((size_t)snapshot->state < STATUS_STATE_COUNT)
    {
        ++observer->status_states[snapshot->state];
    }
    observer->last_scope = scope;
    observer->last_operation_kind = operation_kind;
    observer->status = *snapshot;
    (void)pthread_mutex_unlock(&observer->lock);
}

static esp_err_t _ui_capture_worker(void *argument)
{
    (void)argument;
    s_ui_worker = xTaskGetCurrentTaskHandle();
    return s_ui_worker != NULL && app_manager_mailbox_is_worker() ?
           ESP_OK : ESP_FAIL;
}

static esp_err_t _ui_barrier(void *argument)
{
    (void)argument;
    return app_manager_mailbox_is_worker() ? ESP_OK : ESP_FAIL;
}

static esp_err_t _ui_open_adapter(void *argument)
{
    open_args_t *args = argument;
    const setup_wifi_adapter_callbacks_t callbacks =
    {
        .status = _observer_status,
    };
    return setup_wifi_adapter_open(args->adapter, &callbacks, args->observer);
}

static esp_err_t _ui_close_adapter(void *argument)
{
    return setup_wifi_adapter_close(argument);
}

static esp_err_t _ui_disconnect(void *argument)
{
    return setup_wifi_adapter_disconnect(argument);
}

static esp_err_t _ui_reconnect(void *argument)
{
    return setup_wifi_adapter_reconnect_saved(argument);
}

static esp_err_t _ui_auto_off(void *argument)
{
    return setup_wifi_adapter_set_auto_connect(argument, false);
}

static esp_err_t _ui_auto_on(void *argument)
{
    return setup_wifi_adapter_set_auto_connect(argument, true);
}

static bool _run_on_ui(app_manager_ui_call_fn callback, void *argument)
{
    return app_manager_mailbox_call(callback, argument,
                                    UI_TIMEOUT_MS) == ESP_OK;
}

static bool _wait_status(connectivity_manager_state_t state,
                         connectivity_manager_status_snapshot_t *output)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t snapshot;
        if (connectivity_manager_get_status(&snapshot) == ESP_OK &&
                snapshot.state == state)
        {
            if (output != NULL)
            {
                *output = snapshot;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_status_failure(
    connectivity_manager_state_t state,
    connectivity_manager_failure_t failure,
    connectivity_manager_status_snapshot_t *output)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t snapshot;
        if (connectivity_manager_get_status(&snapshot) == ESP_OK &&
                snapshot.state == state && snapshot.failure == failure)
        {
            if (output != NULL)
            {
                *output = snapshot;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_status_operation(
    connectivity_manager_operation_id_t operation_id, bool complete)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t snapshot;
        if (connectivity_manager_get_status(&snapshot) == ESP_OK &&
                snapshot.operation_id == operation_id &&
                snapshot.operation_complete == complete)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_scan(bool running, esp_err_t result,
                       connectivity_manager_scan_snapshot_t *output)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_scan_snapshot_t snapshot;
        if (connectivity_manager_get_scan_snapshot(&snapshot) == ESP_OK &&
                snapshot.running == running &&
                (running || snapshot.last_error == result))
        {
            if (output != NULL)
            {
                *output = snapshot;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_scan_operation(
    connectivity_manager_operation_id_t operation_id, bool running)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_scan_snapshot_t snapshot;
        if (connectivity_manager_get_scan_snapshot(&snapshot) == ESP_OK &&
                snapshot.operation_id == operation_id &&
                snapshot.running == running)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_auto_connect(bool enabled)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t snapshot;
        if (connectivity_manager_get_status(&snapshot) == ESP_OK &&
                snapshot.auto_connect == enabled)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_operation_result(
    connectivity_manager_operation_id_t operation_id, esp_err_t result)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t snapshot;
        if (connectivity_manager_get_status(&snapshot) == ESP_OK &&
                snapshot.operation_id == operation_id &&
                snapshot.operation_complete &&
                snapshot.last_error == result)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _observer_identity_ok(observer_t *observer)
{
    (void)pthread_mutex_lock(&observer->lock);
    const bool valid = !observer->identity_violation;
    (void)pthread_mutex_unlock(&observer->lock);
    return valid;
}

static esp_err_t _submit_event(wifi_service_port_event_type_t type,
                               wifi_service_failure_t failure,
                               uint32_t ipv4_address)
{
    const wifi_service_port_event_t event =
    {
        .type = type,
        .epoch = host_wifi_port_epoch(),
        .status = failure == WIFI_SERVICE_FAILURE_NONE ? ESP_OK : ESP_FAIL,
        .failure = failure,
        .ipv4_address = ipv4_address,
        .scan_id = host_wifi_port_scan_id(),
    };
    return wifi_service_port_submit_event(&event);
}

static esp_err_t _submit_event_with_epoch(
    wifi_service_port_event_type_t type, uint64_t epoch)
{
    const wifi_service_port_event_t event =
    {
        .type = type,
        .epoch = epoch,
        .status = ESP_OK,
        .scan_id = host_wifi_port_scan_id(),
    };
    return wifi_service_port_submit_event(&event);
}

static bool _wait_wifi_service_status(wifi_service_state_t state)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t snapshot;
        if (wifi_service_get_status(&snapshot) == ESP_OK &&
                snapshot.state == state)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_wifi_service_error(esp_err_t result)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t snapshot;
        if (wifi_service_get_status(&snapshot) == ESP_OK &&
                snapshot.last_error == result)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static void _set_scan_record(const char *ssid,
                             wifi_service_security_t security)
{
    wifi_service_port_scan_record_t record;
    memset(&record, 0, sizeof(record));
    const size_t length = strlen(ssid);
    memcpy(record.ssid, ssid, length);
    record.ssid_length = (uint8_t)length;
    record.rssi = -42;
    record.channel = 6U;
    record.security = security;
    host_wifi_port_set_scan_records(&record, 1U, false);
}

static bool _complete_connection(uint32_t ipv4_address)
{
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_CONNECTED,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_WAITING_IP, NULL));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_GOT_IP,
                        WIFI_SERVICE_FAILURE_NONE, ipv4_address) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IP_READY, NULL));
    return true;
}

static bool _wait_driver_credentials(
    wifi_service_port_credentials_t *credentials)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (host_wifi_port_last_credentials(credentials))
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _request_personal_connect(
    const char *ssid, const char *password,
    connectivity_manager_operation_id_t *operation_id)
{
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = ssid,
        .ssid_length = strlen(ssid),
        .password = password,
        .password_length = strlen(password),
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    return connectivity_manager_request_connect(
               &credentials, operation_id) == ESP_OK;
}

static bool _test_wifi_cancel_dispositions(void)
{
    const wifi_service_config_t config =
    {
        .task_priority = 4U,
    };
    wifi_service_session_id_t session_id = 0U;
    wifi_service_operation_id_t disconnect_id = 0U;
    wifi_service_operation_id_t scan_id = 0U;
    wifi_service_cancel_disposition_t disposition =
        WIFI_SERVICE_CANCEL_FAILURE;

    host_wifi_port_reset();
    disposition = WIFI_SERVICE_CANCEL_ACCEPTED;
    CHECK(wifi_service_cancel(1U, 1U, &disposition) ==
          ESP_ERR_INVALID_STATE);
    CHECK(disposition == WIFI_SERVICE_CANCEL_FAILURE);
    disposition = WIFI_SERVICE_CANCEL_ACCEPTED;
    CHECK(wifi_service_cancel(0U, 1U, &disposition) ==
          ESP_ERR_INVALID_ARG);
    CHECK(disposition == WIFI_SERVICE_CANCEL_FAILURE);
    CHECK(wifi_service_init(&config) == ESP_OK);
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_IDLE));
    CHECK(wifi_service_session_open(&session_id) == ESP_OK);
    CHECK(wifi_service_cancel(session_id, UINT64_MAX, &disposition) ==
          ESP_OK);
    CHECK(disposition == WIFI_SERVICE_CANCEL_NOT_FOUND);

    host_wifi_port_gate(HOST_WIFI_PORT_STOP, true);
    CHECK(wifi_service_request_disconnect(session_id, &disconnect_id) ==
          ESP_OK);
    CHECK(host_wifi_port_wait_gate(HOST_WIFI_PORT_STOP,
                                   TEST_TIMEOUT_MS));
    CHECK(wifi_service_cancel(session_id, disconnect_id, &disposition) ==
          ESP_OK);
    CHECK(disposition == WIFI_SERVICE_CANCEL_TERMINAL_ALREADY_CLAIMED);
    host_wifi_port_release_gate(HOST_WIFI_PORT_STOP);

    bool disconnect_complete = false;
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t snapshot;

        if (wifi_service_get_status(&snapshot) == ESP_OK &&
                snapshot.operation_id == disconnect_id && snapshot.operation_complete &&
                snapshot.operation_result == ESP_OK &&
                snapshot.operation_failure == WIFI_SERVICE_FAILURE_NONE)
        {
            CHECK(!snapshot.operation_canceled);
            disconnect_complete = true;
            break;
        }
        _sleep_one_ms();
    }
    CHECK(disconnect_complete);
    CHECK(wifi_service_cancel(session_id, disconnect_id, &disposition) ==
          ESP_OK);
    CHECK(disposition == WIFI_SERVICE_CANCEL_NOT_FOUND);

    CHECK(wifi_service_request_scan(session_id, &scan_id) == ESP_OK);
    CHECK(wifi_service_cancel(session_id, scan_id, &disposition) == ESP_OK);
    CHECK(disposition == WIFI_SERVICE_CANCEL_ACCEPTED);
    bool scan_canceled = false;
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_scan_snapshot_t snapshot;

        if (wifi_service_get_scan_snapshot(&snapshot) == ESP_OK &&
                snapshot.operation_id == scan_id &&
                snapshot.state == WIFI_SERVICE_SCAN_CANCELED)
        {
            CHECK(snapshot.operation_canceled);
            scan_canceled = true;
            break;
        }
        _sleep_one_ms();
    }
    CHECK(scan_canceled);
    CHECK(wifi_service_session_close(session_id) == ESP_OK);
    CHECK(wifi_service_deinit(WIFI_SERVICE_WAIT_FOREVER) == ESP_OK);
    disposition = WIFI_SERVICE_CANCEL_ACCEPTED;
    CHECK(wifi_service_cancel(session_id, scan_id, &disposition) == ESP_OK);
    CHECK(disposition == WIFI_SERVICE_CANCEL_NOT_FOUND);
    CHECK(host_wifi_port_is_clean_snapshot());
    return true;
}

static bool _wait_driver_ssid(const char *ssid)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_port_credentials_t credentials;
        if (host_wifi_port_last_credentials(&credentials) &&
                credentials.ssid_length == strlen(ssid) &&
                memcmp(credentials.ssid, ssid,
                       credentials.ssid_length) == 0)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _test_operation_arbitration(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t first = 0U;
    connectivity_manager_operation_id_t pending = 0U;
    connectivity_manager_operation_id_t replacement = 0U;
    CHECK(_request_personal_connect("First AP", "password1", &first));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_request_personal_connect("Pending AP", "password2", &pending));
    CHECK(_request_personal_connect("Replacement AP", "password3",
                                    &replacement));
    CHECK(_wait_terminal(pending, ESP_ERR_NOT_FINISHED, false));
    CHECK(_wait_terminal(first, ESP_ERR_NOT_FINISHED, false));
    CHECK(_wait_driver_ssid("Replacement AP"));
    CHECK(_complete_connection(UINT32_C(0x0602a8c0)));
    CHECK(_wait_terminal(replacement, ESP_OK, false));
    CHECK(_terminal_observer_count(first, ESP_ERR_NOT_FINISHED, false) == 1U);
    CHECK(_terminal_observer_count(pending, ESP_ERR_NOT_FINISHED, false) == 1U);
    CHECK(_terminal_observer_count(replacement, ESP_OK, false) == 1U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(wifi_service_test_credentials_are_zero());

    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    connectivity_manager_operation_id_t active_scan = 0U;
    connectivity_manager_operation_id_t connect = 0U;
    connectivity_manager_operation_id_t rejected_scan = 0U;
    CHECK(connectivity_manager_request_scan(&active_scan) == ESP_OK);
    CHECK(_wait_scan_operation(active_scan, true));
    CHECK(_request_personal_connect("Priority AP", "password4", &connect));
    CHECK(connectivity_manager_request_scan(&rejected_scan) == ESP_OK);
    CHECK(_wait_terminal(rejected_scan, ESP_ERR_INVALID_STATE, true));
    CHECK(_wait_terminal(active_scan, ESP_ERR_NOT_FINISHED, true));
    CHECK(_wait_driver_ssid("Priority AP"));
    CHECK(_complete_connection(UINT32_C(0x0702a8c0)));
    CHECK(_wait_terminal(connect, ESP_OK, false));
    CHECK(_terminal_observer_count(active_scan, ESP_ERR_NOT_FINISHED, true) ==
          1U);
    CHECK(_terminal_observer_count(rejected_scan, ESP_ERR_INVALID_STATE,
                                   true) == 1U);
    CHECK(_terminal_observer_count(connect, ESP_OK, false) == 1U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_candidate_terminal_cleanup(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t operation_id = 0U;
    CHECK(_request_personal_connect("Wrong Password AP", "badpass1",
                                    &operation_id));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AUTHENTICATION, 0U) == ESP_OK);
    CHECK(_wait_terminal(operation_id, ESP_FAIL, false));
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION,
                               NULL));
    const unsigned connect_count =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    for (unsigned attempt = 0U; attempt < 300U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connect_count);
    CHECK(_terminal_observer_count(operation_id, ESP_FAIL, false) == 1U);
    CHECK(wifi_service_test_credentials_are_zero());

    const unsigned transient_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(_request_personal_connect("Missing Candidate", "candidate1",
                                    &operation_id));
    for (unsigned attempt = 0U; attempt < 4U; ++attempt)
    {
        CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                        transient_connects + attempt + 1U,
                                        TEST_TIMEOUT_MS));
        CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                            WIFI_SERVICE_FAILURE_AP_NOT_FOUND, 0U) == ESP_OK);
    }
    connectivity_manager_status_snapshot_t retry;
    CHECK(_wait_long_retry(CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND,
                           &retry));
    CHECK(retry.operation_id == operation_id);
    CHECK(!retry.operation_complete);
    CHECK(connectivity_manager_cancel(operation_id) == ESP_OK);
    CHECK(_wait_terminal(operation_id, ESP_ERR_NOT_FINISHED, false));
    CHECK(_terminal_observer_count(operation_id, ESP_ERR_NOT_FINISHED,
                                   false) == 1U);
    CHECK(wifi_service_test_credentials_are_zero());
    const unsigned canceled_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    host_freertos_advance_ticks(1500U);
    for (unsigned attempt = 0U; attempt < 50U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          canceled_connects);

    CHECK(_request_personal_connect("Preempted Candidate", "candidate2",
                                    &operation_id));
    const unsigned preempt_connects = canceled_connects;
    for (unsigned attempt = 0U; attempt < 4U; ++attempt)
    {
        CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                        preempt_connects + attempt + 1U,
                                        TEST_TIMEOUT_MS));
        CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                            WIFI_SERVICE_FAILURE_ASSOCIATION_TIMEOUT,
                            0U) == ESP_OK);
    }
    CHECK(_wait_long_retry(
              CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT, &retry));
    connectivity_manager_operation_id_t disconnect = 0U;
    CHECK(connectivity_manager_request_disconnect(&disconnect) == ESP_OK);
    CHECK(_wait_terminal(operation_id, ESP_ERR_NOT_FINISHED, false));
    CHECK(_wait_terminal(disconnect, ESP_OK, false));
    CHECK(_terminal_observer_count(operation_id, ESP_ERR_NOT_FINISHED,
                                   false) == 1U);
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    return true;
}

static bool _test_cancel_failure_terminal(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t operation_id = 0U;
    CHECK(_request_personal_connect("Cancel Failure AP", "password1",
                                    &operation_id));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_wait_driver_ssid("Cancel Failure AP"));

    /* Queue admission is not a cancellation acknowledgement.  Force the
     * worker's credential cleanup to fail and require a failed terminal. */
    host_wifi_port_gate(HOST_WIFI_PORT_STOP, true);
    CHECK(connectivity_manager_cancel(operation_id) == ESP_OK);
    CHECK(host_wifi_port_wait_gate(HOST_WIFI_PORT_STOP, TEST_TIMEOUT_MS));
    host_wifi_port_fail_next(HOST_WIFI_PORT_CLEAR_CREDENTIALS, 1U, ESP_FAIL);
    host_wifi_port_release_gate(HOST_WIFI_PORT_STOP);
    const bool cancel_terminal = _wait_terminal(operation_id, ESP_FAIL, false);
    CHECK(cancel_terminal);

    terminal_record_t terminal;
    CHECK(_terminal_observer_copy(operation_id, false, &terminal));
    CHECK(terminal.result == ESP_FAIL);
    CHECK(terminal.failure == CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
    CHECK(!terminal.operation_canceled);
    CHECK(!terminal.operation_conflict);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(wifi_service_test_credentials_are_zero());
    return true;
}

static bool _test_save_profile_does_not_connect(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    const unsigned connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = "Saved AP",
        .ssid_length = sizeof("Saved AP") - 1U,
        .password = "password1",
        .password_length = sizeof("password1") - 1U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t operation_id = 0U;
    connectivity_manager_status_snapshot_t status;

    CHECK(connectivity_manager_request_save_profile(
              &credentials, &operation_id) == ESP_OK);
    CHECK(_wait_terminal(operation_id, ESP_OK, false));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.saved_profile);
    CHECK(status.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_missing_profile_terminals(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t reconnect = 0U;
    connectivity_manager_operation_id_t forget = 0U;
    connectivity_manager_operation_id_t set_auto = 0U;
    CHECK(connectivity_manager_request_reconnect_saved(&reconnect) == ESP_OK);
    CHECK(_wait_terminal(reconnect, ESP_ERR_NOT_FOUND, false));
    CHECK(connectivity_manager_request_forget(&forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_ERR_NOT_FOUND, false));
    CHECK(connectivity_manager_set_auto_connect(true, &set_auto) == ESP_OK);
    CHECK(_wait_terminal(set_auto, ESP_ERR_NOT_FOUND, false));

    const connectivity_manager_operation_id_t operations[] =
    {
        reconnect,
        forget,
        set_auto,
    };
    for (size_t index = 0U;
            index < sizeof(operations) / sizeof(operations[0]); ++index)
    {
        terminal_record_t terminal;
        CHECK(_terminal_observer_copy(operations[index], false, &terminal));
        CHECK(terminal.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);
        CHECK(!terminal.operation_canceled);
        CHECK(!terminal.operation_conflict);
    }
    CHECK(host_nv_storage_erase_count() == 0U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    return true;
}

static bool _test_hex_psk_recovery(void)
{
    static const char hex_psk[] =
        "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";
    static const char ssid[] = "Hex PSK AP";
    const size_t hex_length = sizeof(hex_psk) - 1U;
    CHECK(hex_length == 64U);

    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = ssid,
        .ssid_length = sizeof(ssid) - 1U,
        .password = hex_psk,
        .password_length = hex_length,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t operation_id = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &credentials, UINT64_C(0xfeed), true, &operation_id) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    wifi_service_port_credentials_t driver_credentials;
    CHECK(_wait_driver_credentials(&driver_credentials));
    CHECK(driver_credentials.password_length == hex_length);
    CHECK(memcmp(driver_credentials.password, hex_psk, hex_length) == 0);
    CHECK(_complete_connection(UINT32_C(0x0a02a8c0)));
    CHECK(_wait_terminal(operation_id, ESP_OK, false));
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IP_READY, &status));
    CHECK(status.saved_profile);
    CHECK(status.profile_persisted);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    /* A fresh service/radio instance must reload the same 64-byte ASCII PSK. */
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_wait_driver_credentials(&driver_credentials));
    CHECK(driver_credentials.password_length == hex_length);
    CHECK(memcmp(driver_credentials.password, hex_psk, hex_length) == 0);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(wifi_service_test_credentials_are_zero());
    return true;
}

static bool _test_foreground_and_persistence(uint8_t saved_record[],
        size_t *saved_size)
{
    observer_t observer;
    setup_wifi_adapter_t adapter = {0};
    _observer_init(&observer);
    host_nv_storage_reset();
    host_wifi_port_reset();

    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    connectivity_manager_status_snapshot_t initial_status;
    CHECK(connectivity_manager_get_status(&initial_status) == ESP_OK);
    CHECK(!initial_status.saved_profile);

    connectivity_manager_operation_id_t missing_operation = 0U;
    CHECK(connectivity_manager_request_reconnect_saved(
              &missing_operation) == ESP_OK);
    CHECK(_wait_terminal(missing_operation, ESP_ERR_NOT_FOUND, false));
    terminal_record_t terminal;
    CHECK(_terminal_observer_copy(missing_operation, false, &terminal));
    CHECK(terminal.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);
    CHECK(!terminal.operation_canceled);
    CHECK(!terminal.operation_conflict);
    CHECK(connectivity_manager_set_auto_connect(false,
            &missing_operation) == ESP_OK);
    CHECK(_wait_terminal(missing_operation, ESP_ERR_NOT_FOUND, false));
    CHECK(_terminal_observer_copy(missing_operation, false, &terminal));
    CHECK(terminal.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);

    const char invalid_ssid[] = {'A', '\0', 'P'};
    const char valid_password[] = "password1";
    connectivity_manager_credentials_t invalid_credentials =
    {
        .ssid = invalid_ssid,
        .ssid_length = sizeof(invalid_ssid),
        .password = valid_password,
        .password_length = sizeof(valid_password) - 1U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t invalid_operation_id = 0U;
    CHECK(connectivity_manager_request_connect(
              &invalid_credentials, &invalid_operation_id) ==
          ESP_ERR_INVALID_ARG);
    CHECK(invalid_operation_id == 0U);

    const char valid_ssid[] = "Valid AP";
    const char invalid_password[] = {'p', 'a', 's', 's', '\0', 'o', 'r', 'd'};
    invalid_credentials.ssid = valid_ssid;
    invalid_credentials.ssid_length = sizeof(valid_ssid) - 1U;
    invalid_credentials.password = invalid_password;
    invalid_credentials.password_length = sizeof(invalid_password);
    CHECK(connectivity_manager_request_connect(
              &invalid_credentials, &invalid_operation_id) ==
          ESP_ERR_INVALID_ARG);
    CHECK(invalid_operation_id == 0U);
    open_args_t open =
    {
        .adapter = &adapter,
        .observer = &observer,
    };
    CHECK(_run_on_ui(_ui_open_adapter, &open));

    _set_scan_record("Current AP", WIFI_SERVICE_SECURITY_PERSONAL);
    connectivity_manager_operation_id_t scan_operation = 0U;
    CHECK(connectivity_manager_request_scan(&scan_operation) == ESP_OK);
    CHECK(scan_operation != 0U);
    CHECK(_wait_scan(true, ESP_OK, NULL));
    CHECK(_wait_status_operation(scan_operation, false));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    connectivity_manager_scan_snapshot_t scan;
    CHECK(_wait_scan(false, ESP_OK, &scan));
    CHECK(scan.record_count == 1U);
    CHECK(strcmp(scan.records[0].ssid, "Current AP") == 0);
    CHECK(_wait_terminal(scan_operation, ESP_OK, true));
    CHECK(_wait_status_operation(0U, false));

    CHECK(connectivity_manager_request_scan(&scan_operation) == ESP_OK);
    CHECK(_wait_scan(true, ESP_OK, NULL));
    CHECK(connectivity_manager_cancel(scan_operation) == ESP_OK);
    CHECK(_wait_scan(false, ESP_ERR_NOT_FINISHED, &scan));
    CHECK(scan.operation_canceled);
    CHECK(_run_on_ui(_ui_barrier, NULL));

    connectivity_manager_operation_id_t connect_operation = 0U;
    CHECK(_request_personal_connect("Retry AP", "password1",
                                    &connect_operation));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_LINK_LOST, 0U) == ESP_OK);
    connectivity_manager_status_snapshot_t short_retry;
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_RETRY_WAIT, &short_retry));
    CHECK(short_retry.retry_delay_ms == 0U);
    CHECK(_run_on_ui(_ui_barrier, NULL));
    CHECK(connectivity_manager_cancel(connect_operation) == ESP_OK);
    CHECK(_wait_terminal(connect_operation, ESP_ERR_NOT_FINISHED, false));
    CHECK(_terminal_observer_copy(connect_operation, false, &terminal));
    CHECK(terminal.operation_canceled);
    CHECK(!terminal.operation_conflict);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    CHECK(_run_on_ui(_ui_barrier, NULL));

    CHECK(_request_personal_connect("Current AP", "password1",
                                    &connect_operation));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    wifi_service_port_credentials_t driver_credentials;
    CHECK(_wait_driver_credentials(&driver_credentials));
    CHECK(driver_credentials.password_length == 9U);
    CHECK(memcmp(driver_credentials.password, "password1", 9U) == 0);
    CHECK(_complete_connection(UINT32_C(0x0102a8c0)));
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IP_READY, &status));
    CHECK(status.saved_profile);
    CHECK(status.profile_persisted);
    CHECK(status.auto_connect);
    CHECK(host_nv_storage_set_count() == 1U);
    CHECK(host_nv_storage_copy(saved_record, TEST_PROFILE_BYTES, saved_size));
    CHECK(*saved_size > 0U);
    CHECK(_run_on_ui(_ui_barrier, NULL));

    const connectivity_manager_credentials_t sync_credentials =
    {
        .ssid = "Current AP",
        .ssid_length = sizeof("Current AP") - 1U,
        .password = "password1",
        .password_length = sizeof("password1") - 1U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    const unsigned sync_connect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    connectivity_manager_operation_id_t sync_operation = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &sync_credentials, UINT64_C(0x1234), true,
              &sync_operation) == ESP_OK);
    CHECK(sync_operation != 0U);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    sync_connect_before + 1U,
                                    TEST_TIMEOUT_MS));
    CHECK(_complete_connection(UINT32_C(0x0120a8c0)));
    CHECK(_wait_terminal(sync_operation, ESP_OK, false));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IP_READY, &status));
    CHECK(status.profile_revision == 2U);
    CHECK(status.applied_client_sync_id == UINT64_C(0x1234));
    CHECK(host_nv_storage_copy(saved_record, TEST_PROFILE_BYTES, saved_size));

    const unsigned idempotent_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    connectivity_manager_operation_id_t repeat_sync_operation = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &sync_credentials, UINT64_C(0x1234), true,
              &repeat_sync_operation) == ESP_OK);
    CHECK(_wait_terminal(repeat_sync_operation, ESP_OK, false));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          idempotent_connects);

    connectivity_manager_operation_id_t window_auto_off = 0U;
    CHECK(connectivity_manager_set_auto_connect(
              false, &window_auto_off) == ESP_OK);
    CHECK(_wait_terminal(window_auto_off, ESP_OK, false));
    CHECK(_wait_auto_connect(false));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.profile_revision == 3U);
    connectivity_manager_operation_id_t repeat_after_auto = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &sync_credentials, UINT64_C(0x1234), true,
              &repeat_after_auto) == ESP_OK);
    CHECK(_wait_terminal(repeat_after_auto, ESP_OK, false));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.auto_connect);
    CHECK(status.profile_revision == 3U);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          idempotent_connects);
    connectivity_manager_operation_id_t window_auto_on = 0U;
    CHECK(connectivity_manager_set_auto_connect(
              true, &window_auto_on) == ESP_OK);
    CHECK(_wait_terminal(window_auto_on, ESP_OK, false));
    CHECK(_wait_auto_connect(true));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.profile_revision == 4U);
    const unsigned policy_writes = host_nv_storage_set_count();
    connectivity_manager_operation_id_t unchanged_auto = 0U;

    CHECK(connectivity_manager_set_auto_connect(
              true, &unchanged_auto) == ESP_OK);
    CHECK(_wait_terminal(unchanged_auto, ESP_OK, false));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.profile_revision == 4U);
    CHECK(host_nv_storage_set_count() == policy_writes);
    CHECK(host_nv_storage_copy(saved_record, TEST_PROFILE_BYTES, saved_size));

    const connectivity_manager_credentials_t conflicting_sync =
    {
        .ssid = "Current AP",
        .ssid_length = sizeof("Current AP") - 1U,
        .password = "different1",
        .password_length = sizeof("different1") - 1U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t conflict_operation = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &conflicting_sync, UINT64_C(0x1234), true,
              &conflict_operation) == ESP_OK);
    CHECK(_wait_terminal(conflict_operation, ESP_ERR_INVALID_STATE, false));
    CHECK(_terminal_observer_count(
              conflict_operation, ESP_ERR_INVALID_STATE, false) == 1U);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          idempotent_connects);

    connectivity_manager_operation_id_t auto_conflict_operation = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &sync_credentials, UINT64_C(0x1234), false,
              &auto_conflict_operation) == ESP_OK);
    CHECK(_wait_terminal(auto_conflict_operation, ESP_ERR_INVALID_STATE,
                         false));
    CHECK(_terminal_observer_copy(auto_conflict_operation, false, &terminal));
    CHECK(terminal.operation_conflict);
    CHECK(!terminal.operation_canceled);
    CHECK(terminal.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          idempotent_connects);

    CHECK(_run_on_ui(_ui_disconnect, &adapter));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(status.manual_hold);
    CHECK(status.saved_profile);
    CHECK(_run_on_ui(_ui_barrier, NULL));

    _set_scan_record("Current AP", WIFI_SERVICE_SECURITY_OPEN);
    CHECK(connectivity_manager_request_scan(&scan_operation) == ESP_OK);
    CHECK(_wait_scan(true, ESP_OK, NULL));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    CHECK(_wait_scan(false, ESP_OK, &scan));
    CHECK(scan.record_count == 1U);
    CHECK(!scan.records[0].saved);

    _set_scan_record("Current AP", WIFI_SERVICE_SECURITY_PERSONAL);
    CHECK(connectivity_manager_request_scan(&scan_operation) == ESP_OK);
    CHECK(_wait_scan(true, ESP_OK, NULL));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    CHECK(_wait_scan(false, ESP_OK, &scan));
    CHECK(scan.record_count == 1U);
    CHECK(scan.records[0].saved);

    CHECK(_run_on_ui(_ui_reconnect, &adapter));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_complete_connection(UINT32_C(0x0202a8c0)));
    CHECK(_run_on_ui(_ui_barrier, NULL));

    CHECK(_run_on_ui(_ui_auto_off, &adapter));
    CHECK(_wait_auto_connect(false));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IP_READY, NULL));
    CHECK(_run_on_ui(_ui_barrier, NULL));

    const unsigned suspend_connect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(strcmp(status.ssid, "Current AP") == 0);
    CHECK(status.saved_profile);
    CHECK(status.profile_persisted);
    for (unsigned attempt = 0U; attempt < 100U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          suspend_connect_before);

    CHECK(_run_on_ui(_ui_reconnect, &adapter));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_complete_connection(UINT32_C(0x0250a8c0)));
    CHECK(_wait_auto_connect(false));

    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_LINK_LOST, 0U) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(!status.auto_connect);

    CHECK(_run_on_ui(_ui_auto_on, &adapter));
    CHECK(_wait_auto_connect(true));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_complete_connection(UINT32_C(0x0302a8c0)));
    CHECK(_run_on_ui(_ui_barrier, NULL));
    CHECK(host_nv_storage_copy(saved_record, TEST_PROFILE_BYTES, saved_size));

    host_nv_storage_fail_next_set(ESP_FAIL);
    CHECK(_request_personal_connect("Candidate AP", "candidate1",
                                    &connect_operation));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_complete_connection(UINT32_C(0x0402a8c0)));
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IP_READY,
                               CONNECTIVITY_MANAGER_FAILURE_STORAGE,
                               &status));
    CHECK(strcmp(status.ssid, "Candidate AP") == 0);
    CHECK(status.saved_profile);
    CHECK(!status.profile_persisted);
    uint8_t retained[TEST_PROFILE_BYTES];
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == *saved_size);
    CHECK(memcmp(retained, saved_record, retained_size) == 0);

    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_LINK_LOST, 0U) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(strcmp(status.ssid, "Current AP") == 0);
    CHECK(status.saved_profile);
    CHECK(!status.profile_persisted);
    const unsigned connect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    for (unsigned attempt = 0U; attempt < 300U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connect_before);

    CHECK(_run_on_ui(_ui_reconnect, &adapter));
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    connect_before + 1U, TEST_TIMEOUT_MS));
    CHECK(host_wifi_port_last_credentials(&driver_credentials));
    CHECK(driver_credentials.ssid_length == sizeof("Current AP") - 1U);
    CHECK(memcmp(driver_credentials.ssid, "Current AP",
                 driver_credentials.ssid_length) == 0);
    CHECK(_complete_connection(UINT32_C(0x0502a8c0)));
    CHECK(_run_on_ui(_ui_barrier, NULL));

    host_nv_storage_fail_next_erase(ESP_FAIL);
    connectivity_manager_operation_id_t failed_forget_operation_id = 0U;
    CHECK(connectivity_manager_request_forget(
              &failed_forget_operation_id) == ESP_OK);
    CHECK(_wait_operation_result(failed_forget_operation_id, ESP_FAIL));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(status.saved_profile);
    CHECK(status.manual_hold);
    CHECK(host_nv_storage_erase_count() == 0U);

    connectivity_manager_operation_id_t foreground_scan_operation_id = 0U;
    CHECK(connectivity_manager_request_scan(
              &foreground_scan_operation_id) == ESP_OK);
    CHECK(foreground_scan_operation_id != 0U);
    CHECK(_wait_scan_operation(foreground_scan_operation_id, true));
    host_wifi_port_gate(HOST_WIFI_PORT_SCAN_ABORT, true);
    connectivity_manager_operation_id_t forget_operation_id = 0U;
    CHECK(connectivity_manager_request_forget(&forget_operation_id) == ESP_OK);
    CHECK(forget_operation_id != 0U);
    CHECK(host_wifi_port_wait_gate(HOST_WIFI_PORT_SCAN_ABORT,
                                   TEST_TIMEOUT_MS));
    CHECK(host_nv_storage_erase_count() == 0U);

    connectivity_manager_operation_id_t disconnect_operation_id = 0U;
    CHECK(connectivity_manager_request_disconnect(
              &disconnect_operation_id) == ESP_OK);
    CHECK(disconnect_operation_id != 0U);
    CHECK(_wait_operation_result(disconnect_operation_id,
                                 ESP_ERR_INVALID_STATE));
    CHECK(host_nv_storage_erase_count() == 0U);
    host_wifi_port_release_gate(HOST_WIFI_PORT_SCAN_ABORT);

    CHECK(_wait_operation_result(forget_operation_id, ESP_OK));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(!status.saved_profile);
    CHECK(status.manual_hold);
    CHECK(host_nv_storage_erase_count() == 1U);
    CHECK(!host_nv_storage_copy(NULL, 0U, NULL));
    CHECK(wifi_service_test_credentials_are_zero());

    CHECK(_run_on_ui(_ui_close_adapter, &adapter));
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(_observer_identity_ok(&observer));
    _observer_deinit(&observer);
    return true;
}

static bool _wait_long_retry(connectivity_manager_failure_t failure,
                             connectivity_manager_status_snapshot_t *output)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t snapshot;
        if (connectivity_manager_get_status(&snapshot) == ESP_OK &&
                snapshot.state == CONNECTIVITY_MANAGER_STATE_RETRY_WAIT &&
                snapshot.failure == failure &&
                snapshot.retry_delay_ms == 1000U)
        {
            if (output != NULL)
            {
                *output = snapshot;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _test_boot_retry_timeouts(const uint8_t saved_record[],
                                      size_t saved_size)
{
    host_nv_storage_seed(saved_record, saved_size);
    const unsigned connect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    connect_before + 1U, TEST_TIMEOUT_MS));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));

    CHECK(_submit_event_with_epoch(
              WIFI_SERVICE_PORT_EVENT_GOT_IP,
              host_wifi_port_epoch() + 1U) == ESP_OK);
    for (unsigned attempt = 0U; attempt < 50U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));

    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AUTHENTICATION, 0U) == ESP_OK);
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION,
                               &status));
    const unsigned auth_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    for (unsigned attempt = 0U; attempt < 300U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == auth_connects);

    connectivity_manager_operation_id_t operation_id = 0U;
    CHECK(connectivity_manager_request_reconnect_saved(&operation_id) == ESP_OK);
    CHECK(operation_id != 0U);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_wait_long_retry(
              CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT, &status));
    CHECK(status.retry_count == 1U);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          auth_connects + 4U);

    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
    const unsigned suspend_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    host_freertos_advance_ticks(1500U);
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    suspend_connects + 1U, TEST_TIMEOUT_MS));
    for (unsigned attempt = 0U; attempt < 100U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          suspend_connects + 1U);
    CHECK(_wait_status_operation(operation_id, false));
    CHECK(_terminal_observer_count(operation_id, ESP_OK, false) == 0U);
    printf("retry recovery: id=%llu deadline crossed in suspend, exactly one resumed attempt\n",
           (unsigned long long)operation_id);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AUTHENTICATION, 0U) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION,
                               NULL));

    const unsigned dhcp_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_request_reconnect_saved(&operation_id) == ESP_OK);
    for (unsigned attempt = 0U; attempt < 4U; ++attempt)
    {
        CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                        dhcp_connects + attempt + 1U,
                                        TEST_TIMEOUT_MS));
        CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
        CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_CONNECTED,
                            WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_WAITING_IP, NULL));
    }
    CHECK(_wait_long_retry(CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT, &status));
    CHECK(status.retry_count == 1U);

    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_active_radio_failure_terminal(const uint8_t saved_record[],
        size_t saved_size)
{
    host_nv_storage_reset();
    host_nv_storage_seed(saved_record, saved_size);
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AUTHENTICATION, 0U) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION,
                               NULL));

    connectivity_manager_operation_id_t reconnect = 0U;
    host_wifi_port_fail_next(HOST_WIFI_PORT_START, 1U, ESP_FAIL);
    CHECK(connectivity_manager_request_reconnect_saved(&reconnect) == ESP_OK);
    CHECK(_wait_terminal(reconnect, ESP_FAIL, false));
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_RETRY_WAIT,
                               CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE,
                               &status));
    CHECK(!status.radio_available);
    CHECK(_terminal_observer_count(reconnect, ESP_FAIL, false) == 1U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(wifi_service_test_credentials_are_zero());
    return true;
}

static bool _test_scan_resume_guards(const uint8_t saved_record[],
                                     size_t saved_size)
{
    host_nv_storage_reset();
    host_nv_storage_seed(saved_record, saved_size);
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    connectivity_manager_operation_id_t scan = 0U;
    connectivity_manager_operation_id_t disconnect = 0U;
    CHECK(connectivity_manager_request_scan(&scan) == ESP_OK);
    CHECK(_wait_scan_operation(scan, true));
    CHECK(connectivity_manager_request_disconnect(&disconnect) == ESP_OK);
    CHECK(_wait_terminal(scan, ESP_ERR_NOT_FINISHED, true));
    CHECK(_wait_terminal(disconnect, ESP_OK, false));
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(status.manual_hold);
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(status.manual_hold);
    const unsigned disconnect_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    for (unsigned attempt = 0U; attempt < 200U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          disconnect_connects);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    host_nv_storage_reset();
    host_nv_storage_seed(saved_record, saved_size);
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_scan(&scan) == ESP_OK);
    CHECK(_wait_scan_operation(scan, true));
    CHECK(connectivity_manager_request_forget(&forget) == ESP_OK);
    CHECK(_wait_terminal(scan, ESP_ERR_NOT_FINISHED, true));
    CHECK(_wait_terminal(forget, ESP_OK, false));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(!status.saved_profile);
    const unsigned forget_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    for (unsigned attempt = 0U; attempt < 200U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == forget_connects);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    host_nv_storage_reset();
    host_nv_storage_seed(saved_record, saved_size);
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(connectivity_manager_request_scan(&scan) == ESP_OK);
    CHECK(_wait_scan_operation(scan, true));
    connectivity_manager_operation_id_t auto_off = 0U;
    CHECK(connectivity_manager_set_auto_connect(false, &auto_off) == ESP_OK);
    CHECK(_wait_terminal(auto_off, ESP_OK, false));
    CHECK(_wait_auto_connect(false));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    CHECK(_wait_terminal(scan, ESP_OK, true));
    const unsigned disabled_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    for (unsigned attempt = 0U; attempt < 200U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) ==
          disabled_connects);

    connectivity_manager_operation_id_t auto_on = 0U;
    CHECK(connectivity_manager_set_auto_connect(true, &auto_on) == ESP_OK);
    CHECK(_wait_terminal(auto_on, ESP_OK, false));
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    disabled_connects + 1U,
                                    TEST_TIMEOUT_MS));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, &status));
    CHECK(status.operation_id == 0U);
    CHECK(!status.operation_complete);
    CHECK(_complete_connection(UINT32_C(0x0802a8c0)));
    CHECK(_terminal_observer_count(auto_off, ESP_OK, false) == 1U);
    CHECK(_terminal_observer_count(auto_on, ESP_OK, false) == 1U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_scan_suspend_resume(const uint8_t saved_record[],
                                      size_t saved_size)
{
    host_nv_storage_reset();
    host_nv_storage_seed(saved_record, saved_size);
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));

    connectivity_manager_operation_id_t scan = 0U;
    CHECK(connectivity_manager_request_scan(&scan) == ESP_OK);
    CHECK(_wait_scan_operation(scan, true));
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
    CHECK(_wait_terminal(scan, ESP_ERR_NOT_FINISHED, true));
    const unsigned suspended_connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);

    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    suspended_connects + 1U,
                                    TEST_TIMEOUT_MS));
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, &status));
    CHECK(status.operation_id == 0U);
    CHECK(!status.operation_complete);
    CHECK(_terminal_observer_count(scan, ESP_ERR_NOT_FINISHED, true) == 1U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_invalid_profile_record(const uint8_t record[], size_t size)
{
    host_nv_storage_reset();
    host_nv_storage_seed(record, size);
    const unsigned connect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_status_snapshot_t status;
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_INTERNAL,
                               &status));
    CHECK(!status.saved_profile);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connect_before);
    CHECK(host_nv_storage_erase_count() == 0U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    uint8_t retained[TEST_PROFILE_BYTES];
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == size);
    CHECK(memcmp(retained, record, size) == 0);
    return true;
}

static bool _test_profile_read_failure_classification(void)
{
    host_nv_storage_reset();
    host_nv_storage_fail_next_get(ESP_FAIL);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_status_snapshot_t status;

    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_STORAGE,
                               &status));
    CHECK(status.last_error == ESP_FAIL);
    CHECK(!status.saved_profile);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_invalid_profile(const uint8_t saved_record[],
                                  size_t saved_size)
{
    connectivity_manager_status_snapshot_t status;
    uint8_t corrupted[TEST_PROFILE_BYTES];
    CHECK(saved_size == PROFILE_RECORD_BYTES);
    CHECK(saved_size <= sizeof(corrupted));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_VERSION_OFFSET] ^= UINT8_C(0x7f);
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    /* A saved slot whose SSID is empty must be rejected. */
    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_ENTRY_SSID_OFFSET] = 0U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    /* A personal entry with an empty password must be rejected. */
    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_ENTRY_PASSWORD_OFFSET] = 0U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_ENTRY_RESERVED_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_ENTRY_TRAILING_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    /* The count must stay inside the reserved slot array. */
    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_COUNT_OFFSET] = 6U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    /* An empty list cannot select an active entry. */
    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_COUNT_OFFSET] = 0U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    /* The active index must address a saved entry. */
    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_ACTIVE_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_RESERVED_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    memset(corrupted + PROFILE_REVISION_OFFSET, 0,
           sizeof(uint64_t));
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    memset(corrupted + PROFILE_SYNC_DIGEST_OFFSET, 0, 32U);
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_SYNC_VALID_OFFSET] = 2U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_SYNC_RESERVED_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    host_nv_storage_seed(saved_record, saved_size - 1U);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_INTERNAL,
                               &status));
    CHECK(!status.saved_profile);
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(NULL, 0U, &retained_size));
    CHECK(retained_size == saved_size - 1U);
    CHECK(host_nv_storage_erase_count() == 0U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    memcpy(corrupted, saved_record, saved_size);
    corrupted[saved_size] = 0U;
    host_nv_storage_seed(corrupted, saved_size + 1U);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_INTERNAL,
                               &status));
    CHECK(!status.saved_profile);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_sync_window_restart(const uint8_t saved_record[],
                                      size_t saved_size)
{
    uint8_t profile[TEST_PROFILE_BYTES];

    CHECK(saved_size == PROFILE_RECORD_BYTES);
    memcpy(profile, saved_record, saved_size);
    /* This is the persisted state after SetAutoConnect(false): policy
     * changes while the last SetCredentials identity remains intact. */
    profile[PROFILE_AUTO_CONNECT_OFFSET] = 0U;
    memset(&profile[PROFILE_REVISION_OFFSET], 0, sizeof(uint64_t));
    profile[PROFILE_REVISION_OFFSET] = 7U;
    host_nv_storage_reset();
    host_nv_storage_seed(profile, saved_size);
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_status_snapshot_t status;

    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(!status.auto_connect);
    CHECK(status.applied_client_sync_id == UINT64_C(0x1234));
    const unsigned connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = "Current AP",
        .ssid_length = sizeof("Current AP") - 1U,
        .password = "password1",
        .password_length = sizeof("password1") - 1U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t repeat = 0U;

    CHECK(connectivity_manager_request_sync_profile(
              &credentials, UINT64_C(0x1234), true, &repeat) == ESP_OK);
    CHECK(_wait_terminal(repeat, ESP_OK, false));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.auto_connect);

    connectivity_manager_operation_id_t conflict = 0U;
    CHECK(connectivity_manager_request_sync_profile(
              &credentials, UINT64_C(0x1234), false, &conflict) == ESP_OK);
    CHECK(_wait_terminal(conflict, ESP_ERR_INVALID_STATE, false));
    terminal_record_t terminal;
    CHECK(_terminal_observer_copy(conflict, false, &terminal));
    CHECK(terminal.operation_conflict);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

/* Legacy single-profile record offsets. v1/v2/v3 share the header and payload
 * layout; v2 appends the revision and sync id, v3 appends the sync window. */
enum
{
    LEGACY_MAGIC_OFFSET = 0U,
    LEGACY_VERSION_OFFSET = 4U,
    LEGACY_SECURITY_OFFSET = 6U,
    LEGACY_AUTO_CONNECT_OFFSET = 7U,
    LEGACY_SSID_LENGTH_OFFSET = 8U,
    LEGACY_PASSWORD_LENGTH_OFFSET = 9U,
    LEGACY_RESERVED_OFFSET = 10U,
    LEGACY_SSID_OFFSET = 12U,
    LEGACY_PASSWORD_OFFSET = 44U,
    LEGACY_TRAILING_OFFSET = 109U,
    LEGACY_REVISION_OFFSET = 112U,
    LEGACY_SYNC_ID_OFFSET = 120U,
    LEGACY_SYNC_DIGEST_OFFSET = 128U,
    LEGACY_SYNC_VALID_OFFSET = 160U,
    LEGACY_SYNC_RESERVED_OFFSET = 161U,
    LEGACY_V1_BYTES = 112U,
    LEGACY_V2_BYTES = 128U,
    LEGACY_V3_BYTES = 168U,
};

/* Build a legacy single-profile record for the given version. */
static void _build_legacy_record(uint8_t *record, size_t size, uint16_t version)
{
    static const char ssid[] = "Legacy AP";
    static const char password[] = "password1";
    const size_t ssid_length = sizeof(ssid) - 1U;
    const size_t password_length = sizeof(password) - 1U;

    memset(record, 0, size);
    record[LEGACY_MAGIC_OFFSET] = 0x31U;
    record[LEGACY_MAGIC_OFFSET + 1U] = 0x50U;
    record[LEGACY_MAGIC_OFFSET + 2U] = 0x46U;
    record[LEGACY_MAGIC_OFFSET + 3U] = 0x57U;
    record[LEGACY_VERSION_OFFSET] = (uint8_t)(version & 0xffU);
    record[LEGACY_VERSION_OFFSET + 1U] = (uint8_t)(version >> 8);
    record[LEGACY_SECURITY_OFFSET] = (uint8_t)CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
    record[LEGACY_AUTO_CONNECT_OFFSET] = 1U;
    record[LEGACY_SSID_LENGTH_OFFSET] = (uint8_t)ssid_length;
    record[LEGACY_PASSWORD_LENGTH_OFFSET] = (uint8_t)password_length;
    memcpy(&record[LEGACY_SSID_OFFSET], ssid, ssid_length);
    memcpy(&record[LEGACY_PASSWORD_OFFSET], password, password_length);
    if (version >= 2U)
    {
        /* profile_revision = 4 */
        record[LEGACY_REVISION_OFFSET] = 0x04U;
    }
}

static bool _test_legacy_profile_migration(const uint8_t saved_record[],
        size_t saved_size)
{
    (void)saved_record;
    uint8_t legacy[LEGACY_V3_BYTES];
    uint8_t retained[TEST_PROFILE_BYTES];
    size_t retained_size = 0U;
    connectivity_manager_status_snapshot_t status;

    CHECK(saved_size == PROFILE_RECORD_BYTES);

    /* v1 (112 bytes) migrates to one v4 entry with revision 1. */
    _build_legacy_record(legacy, LEGACY_V1_BYTES, 1U);
    host_nv_storage_reset();
    host_nv_storage_seed(legacy, LEGACY_V1_BYTES);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, &status));
    CHECK(status.saved_profile);
    CHECK(status.profile_count == 1U);
    CHECK(host_nv_storage_set_count() == 1U);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == saved_size);
    CHECK(retained[PROFILE_COUNT_OFFSET] == 1U);
    CHECK(retained[PROFILE_ACTIVE_OFFSET] == 0U);
    CHECK(retained[PROFILE_SYNC_VALID_OFFSET] == 0U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    /* A refused migration write keeps the original bytes and reports storage. */
    host_nv_storage_reset();
    host_nv_storage_seed(legacy, LEGACY_V1_BYTES);
    host_nv_storage_fail_next_set(ESP_FAIL);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_STORAGE,
                               &status));
    CHECK(!status.saved_profile);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == LEGACY_V1_BYTES);
    CHECK(memcmp(retained, legacy, LEGACY_V1_BYTES) == 0);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    /* An inconsistent legacy payload is rejected without touching flash. */
    uint8_t invalid[LEGACY_V3_BYTES];
    _build_legacy_record(invalid, sizeof(invalid), 1U);
    invalid[LEGACY_PASSWORD_LENGTH_OFFSET] = 1U;
    memset(invalid + LEGACY_PASSWORD_OFFSET + 1U, 0,
           sizeof(invalid) - LEGACY_PASSWORD_OFFSET - 1U);
    host_nv_storage_reset();
    host_nv_storage_seed(invalid, LEGACY_V1_BYTES);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_INTERNAL,
                               &status));
    CHECK(!status.saved_profile);
    CHECK(host_nv_storage_set_count() == 0U);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == LEGACY_V1_BYTES);
    CHECK(memcmp(retained, invalid, LEGACY_V1_BYTES) == 0);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    /* v2 (128 bytes) migrates with its revision and no sync identity. */
    _build_legacy_record(legacy, LEGACY_V2_BYTES, 2U);
    host_nv_storage_reset();
    host_nv_storage_seed(legacy, LEGACY_V2_BYTES);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, &status));
    CHECK(status.saved_profile);
    CHECK(status.profile_count == 1U);
    CHECK(status.applied_client_sync_id == 0U);
    CHECK(host_nv_storage_set_count() == 1U);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == saved_size);
    CHECK(retained[PROFILE_SYNC_VALID_OFFSET] == 0U);
    for (size_t index = 0U; index < sizeof(uint64_t); ++index)
    {
        CHECK(retained[PROFILE_SYNC_ID_OFFSET + index] == 0U);
    }
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);

    /* v3 (168 bytes) keeps its revision and empty sync window. */
    _build_legacy_record(legacy, LEGACY_V3_BYTES, 3U);
    host_nv_storage_reset();
    host_nv_storage_seed(legacy, LEGACY_V3_BYTES);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, &status));
    CHECK(status.saved_profile);
    CHECK(status.profile_count == 1U);
    CHECK(status.profile_revision == UINT64_C(4));
    CHECK(status.applied_client_sync_id == 0U);
    CHECK(host_nv_storage_set_count() == 1U);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == saved_size);
    CHECK(retained[PROFILE_COUNT_OFFSET] == 1U);
    CHECK(retained[PROFILE_ACTIVE_OFFSET] == 0U);
    CHECK(retained[PROFILE_SYNC_VALID_OFFSET] == 0U);
    for (size_t index = 0U; index < sizeof(uint64_t); ++index)
    {
        CHECK(retained[PROFILE_SYNC_ID_OFFSET + index] == 0U);
    }
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_manager_deinit_retry(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    host_wifi_port_gate(HOST_WIFI_PORT_DEINIT, true);
    CHECK(connectivity_manager_deinit(20U) == ESP_ERR_TIMEOUT);
    CHECK(host_wifi_port_wait_gate(HOST_WIFI_PORT_DEINIT, TEST_TIMEOUT_MS));
    connectivity_manager_operation_id_t operation_id = 0U;
    CHECK(connectivity_manager_request_scan(&operation_id) ==
          ESP_ERR_INVALID_STATE);
    CHECK(operation_id == 0U);
    host_wifi_port_release_gate(HOST_WIFI_PORT_DEINIT);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());

    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    host_wifi_port_fail_next(HOST_WIFI_PORT_DEINIT, 1U, ESP_FAIL);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_FAIL);
    CHECK(connectivity_manager_request_scan(&operation_id) ==
          ESP_ERR_INVALID_STATE);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    return true;
}

static bool _test_manager_deinit_terminal(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t connect = 0U;
    CHECK(_request_personal_connect("Deinit AP", "password1", &connect));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(_wait_terminal(connect, ESP_ERR_NOT_FINISHED, false));
    CHECK(_terminal_observer_count(connect, ESP_ERR_NOT_FINISHED, false) ==
          1U);
    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(wifi_service_test_credentials_are_zero());
    return true;
}

static bool _test_terminal_outbox_deinit_barrier(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    atomic_store_explicit(&s_block_ui_dispatch, false, memory_order_release);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t connect = 0U;
    CHECK(_request_personal_connect("Outbox AP", "password1", &connect));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    atomic_store_explicit(&s_block_ui_dispatch, true, memory_order_release);
    connectivity_manager_operation_id_t rejected_scan = 0U;
    CHECK(connectivity_manager_request_scan(&rejected_scan) == ESP_OK);
    connectivity_manager_scan_snapshot_t cached;
    CHECK(_wait_scan_operation(rejected_scan, false));
    CHECK(connectivity_manager_get_scan_snapshot(&cached) == ESP_OK);
    CHECK(cached.operation_id == rejected_scan);
    CHECK(cached.last_error == ESP_ERR_INVALID_STATE);
    CHECK(_terminal_observer_count(rejected_scan, ESP_ERR_INVALID_STATE,
                                   true) == 0U);

    CHECK(connectivity_manager_deinit(20U) == ESP_ERR_TIMEOUT);
    atomic_store_explicit(&s_block_ui_dispatch, false, memory_order_release);
    CHECK(_wait_terminal(rejected_scan, ESP_ERR_INVALID_STATE, true));
    CHECK(_terminal_observer_generation(rejected_scan, true) ==
          cached.generation);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(_wait_terminal(connect, ESP_ERR_NOT_FINISHED, false));
    CHECK(_terminal_observer_count(rejected_scan, ESP_ERR_INVALID_STATE,
                                   true) == 1U);
    CHECK(_terminal_observer_count(connect, ESP_ERR_NOT_FINISHED, false) ==
          1U);
    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(wifi_service_test_credentials_are_zero());
    return true;
}

static bool _test_manager_init_resource_failures(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    const unsigned live_semaphores = host_freertos_live_semaphores();
    const unsigned live_queues = host_freertos_live_queues();
    const unsigned live_tasks = host_freertos_live_tasks();
    const unsigned init_calls =
        host_wifi_port_call_count(HOST_WIFI_PORT_INIT);

    connectivity_manager_config_t invalid = s_manager_config;
    invalid.task_priority = configMAX_PRIORITIES;
    CHECK(connectivity_manager_init(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = s_manager_config;
    invalid.wifi_task_priority = configMAX_PRIORITIES;
    CHECK(connectivity_manager_init(&invalid) == ESP_ERR_INVALID_ARG);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_INIT) == init_calls);
    CHECK(host_freertos_live_semaphores() == live_semaphores);
    CHECK(host_freertos_live_queues() == live_queues);
    CHECK(host_freertos_live_tasks() == live_tasks);

    host_freertos_fail_semaphore_create_after(1U);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_ERR_NO_MEM);
    CHECK(host_freertos_live_semaphores() == live_semaphores);
    CHECK(host_freertos_live_queues() == live_queues);
    CHECK(host_freertos_live_tasks() == live_tasks);

    host_freertos_reset_controls();
    host_freertos_fail_next_queue_creates(1U);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_ERR_NO_MEM);
    CHECK(host_freertos_live_semaphores() == live_semaphores);
    CHECK(host_freertos_live_queues() == live_queues);
    CHECK(host_freertos_live_tasks() == live_tasks);

    host_freertos_reset_controls();
    host_freertos_fail_next_task_creates(1U);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_ERR_NO_MEM);
    CHECK(host_freertos_live_semaphores() == live_semaphores);
    CHECK(host_freertos_live_queues() == live_queues);
    CHECK(host_freertos_live_tasks() == live_tasks);
    host_freertos_reset_controls();
    return true;
}

static bool _test_clear_persisted_profile_lifecycle(void)
{
    static const uint8_t stored_profile[] =
    {
        0x57U, 0x46U, 0x50U, 0x31U,
    };
    uint8_t retained[sizeof(stored_profile)];
    size_t retained_size = 0U;

    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(connectivity_manager_clear_persisted_profile() == ESP_OK);
    CHECK(connectivity_manager_clear_persisted_profile() == ESP_OK);
    CHECK(host_nv_storage_erase_count() == 0U);

    host_nv_storage_seed(stored_profile, sizeof(stored_profile));
    CHECK(connectivity_manager_clear_persisted_profile() == ESP_OK);
    CHECK(host_nv_storage_erase_count() == 1U);
    CHECK(!host_nv_storage_copy(NULL, 0U, NULL));
    CHECK(connectivity_manager_clear_persisted_profile() == ESP_OK);
    CHECK(host_nv_storage_erase_count() == 1U);

    host_nv_storage_seed(stored_profile, sizeof(stored_profile));
    host_nv_storage_fail_next_erase(ESP_FAIL);
    CHECK(connectivity_manager_clear_persisted_profile() == ESP_FAIL);
    CHECK(host_nv_storage_erase_count() == 1U);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == sizeof(stored_profile));
    CHECK(memcmp(retained, stored_profile, sizeof(stored_profile)) == 0);
    CHECK(connectivity_manager_clear_persisted_profile() == ESP_OK);
    CHECK(host_nv_storage_erase_count() == 2U);

    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    host_nv_storage_seed(stored_profile, sizeof(stored_profile));
    CHECK(connectivity_manager_clear_persisted_profile() ==
          ESP_ERR_INVALID_STATE);
    CHECK(host_nv_storage_erase_count() == 2U);
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == sizeof(stored_profile));
    CHECK(memcmp(retained, stored_profile, sizeof(stored_profile)) == 0);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    return true;
}

static bool _test_queue_overflow(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    host_freertos_defer_queue_delivery(true);
    CHECK(host_freertos_wait_queue_delivery_deferred(TEST_TIMEOUT_MS));
    for (unsigned attempt = 0U; attempt < 50U; ++attempt)
    {
        _sleep_one_ms();
    }
    for (size_t index = 0U;
            index < CONFIG_CONNECTIVITY_MANAGER_QUEUE_DEPTH; ++index)
    {
        connectivity_manager_operation_id_t operation_id = 0U;
        CHECK(connectivity_manager_request_scan(&operation_id) == ESP_OK);
        CHECK(operation_id != 0U);
    }
    connectivity_manager_operation_id_t rejected_operation_id = 0U;
    CHECK(connectivity_manager_request_scan(&rejected_operation_id) ==
          ESP_ERR_NO_MEM);
    CHECK(rejected_operation_id == 0U);
    host_freertos_defer_queue_delivery(false);
    CHECK(host_freertos_wait_queues_empty(TEST_TIMEOUT_MS));
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());

    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    host_freertos_defer_queue_delivery(true);
    CHECK(host_freertos_wait_queue_delivery_deferred(TEST_TIMEOUT_MS));
    for (unsigned attempt = 0U; attempt < 50U; ++attempt)
    {
        _sleep_one_ms();
    }
    for (size_t index = 0U; index < CONFIG_WIFI_SERVICE_QUEUE_DEPTH; ++index)
    {
        CHECK(_submit_event_with_epoch(
                  WIFI_SERVICE_PORT_EVENT_LOST_IP,
                  host_wifi_port_epoch()) == ESP_OK);
    }
    CHECK(_submit_event_with_epoch(
              WIFI_SERVICE_PORT_EVENT_LOST_IP,
              host_wifi_port_epoch()) == ESP_ERR_NO_MEM);
    host_freertos_defer_queue_delivery(false);
    CHECK(_wait_wifi_service_error(ESP_ERR_NO_MEM));
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    return true;
}

static void *_release_binary_give(void *argument)
{
    (void)argument;
    for (unsigned attempt = 0U; attempt < 10U; ++attempt)
    {
        _sleep_one_ms();
    }
    host_freertos_release_binary_give();
    return NULL;
}

static bool _test_control_completion_generation(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    host_wifi_port_fail_next(HOST_WIFI_PORT_INIT, 1U, ESP_FAIL);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_OFFLINE,
                               CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE,
                               NULL));

    host_freertos_block_next_binary_give();
    CHECK(connectivity_manager_suspend(20U) == ESP_ERR_TIMEOUT);
    CHECK(host_freertos_wait_binary_give_blocked(TEST_TIMEOUT_MS));
    pthread_t releaser;
    CHECK(pthread_create(&releaser, NULL,
                         _release_binary_give, NULL) == 0);
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(pthread_join(releaser, NULL) == 0);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    host_freertos_reset_controls();
    return true;
}

static bool _test_pending_sync_profile(bool active_connect, bool block_outbox)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_operation_id_t first = 0U;
    if (active_connect)
    {
        CHECK(_request_personal_connect("Preempt A", "password1", &first));
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    }
    else
    {
        CHECK(connectivity_manager_request_scan(&first) == ESP_OK);
        CHECK(_wait_scan_operation(first, true));
    }
    atomic_store_explicit(&s_block_ui_dispatch, block_outbox, memory_order_release);
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = "Pending sync B", .ssid_length = 14U,
        .password = "password2", .password_length = 9U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t sync = 0U;
    CHECK(connectivity_manager_request_sync_profile(&credentials, UINT64_C(0x6789),
            false, &sync) == ESP_OK);
    if (block_outbox)
    {
        CHECK(_wait_scan_operation(first, false));
        CHECK(_terminal_observer_count(first, ESP_ERR_NOT_FINISHED, true) == 0U);
        CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == 0U);
        atomic_store_explicit(&s_block_ui_dispatch, false, memory_order_release);
    }
    CHECK(_wait_terminal(first, ESP_ERR_NOT_FINISHED, !active_connect));
    /* No new command/event may be needed to start B after terminal drain. */
    CHECK(_wait_driver_ssid("Pending sync B"));
    CHECK(host_nv_storage_set_count() == 0U);
    CHECK(_complete_connection(UINT32_C(0x0A020304)));
    CHECK(_wait_terminal(sync, ESP_OK, false));
    CHECK(_terminal_observer_count(first, ESP_ERR_NOT_FINISHED, !active_connect) == 1U);
    CHECK(_terminal_observer_count(sync, ESP_OK, false) == 1U);
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.applied_client_sync_id == UINT64_C(0x6789) && !status.auto_connect);
    CHECK(host_nv_storage_set_count() == 1U);
    const unsigned connects = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    connectivity_manager_operation_id_t repeat = 0U;
    CHECK(connectivity_manager_request_sync_profile(&credentials, UINT64_C(0x6789),
            false, &repeat) == ESP_OK);
    CHECK(_wait_terminal(repeat, ESP_OK, false));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
    CHECK(host_nv_storage_set_count() == 1U);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    printf("pending sync: preempt=%s outbox=%u id=%llu committed once with sync identity\n",
           active_connect ? "CONNECT" : "SCAN", block_outbox,
           (unsigned long long)sync);
    return true;
}

static bool _wait_radio_terminal(wifi_service_operation_id_t operation,
                                 wifi_service_status_snapshot_t *output)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t snapshot;
        if (wifi_service_get_status(&snapshot) == ESP_OK &&
                snapshot.operation_id == operation && snapshot.operation_complete)
        {
            *output = snapshot;
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_radio_connecting_with_new_operation(
    wifi_service_operation_id_t previous_operation,
    wifi_service_status_snapshot_t *output)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t snapshot;
        if (wifi_service_get_status(&snapshot) == ESP_OK &&
                snapshot.state == WIFI_SERVICE_STATE_CONNECTING &&
                snapshot.operation_id != 0U &&
                snapshot.operation_id != previous_operation)
        {
            if (output != NULL)
            {
                *output = snapshot;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _test_radio_transition_failures(void)
{
    const wifi_service_config_t config = { .task_priority = 4U };
    const wifi_service_credentials_t credentials =
    {
        .ssid = "Transition AP", .ssid_length = 13U,
        .password = "password1", .password_length = 9U,
        .security = WIFI_SERVICE_SECURITY_PERSONAL,
    };
    for (unsigned mode = 0U; mode < 6U; ++mode)
    {
        host_wifi_port_reset();
        CHECK(wifi_service_init(&config) == ESP_OK);
        wifi_service_session_id_t session = 0U;
        wifi_service_operation_id_t operation = 0U;
        CHECK(wifi_service_session_open(&session) == ESP_OK);
        if (mode == 0U || mode == 4U)
        {
            host_wifi_port_fail_next(HOST_WIFI_PORT_STOP, 1U, ESP_FAIL);
            if (mode == 4U)
            {
                host_wifi_port_leave_stopped_after_next_stop_failure();
            }
        }
        else if (mode == 1U)
        {
            host_wifi_port_fail_next(HOST_WIFI_PORT_CLEAR_CREDENTIALS, 1U, ESP_FAIL);
        }
        else
        {
            host_wifi_port_fail_next(HOST_WIFI_PORT_START, 1U, ESP_FAIL);
            if (mode == 3U)
            {
                host_wifi_port_leave_partial_after_next_start_failure();
            }
            else if (mode == 5U)
            {
                host_wifi_port_leave_started_after_next_start_failure();
            }
        }
        CHECK(wifi_service_request_connect(session, &credentials, &operation) == ESP_OK);
        wifi_service_status_snapshot_t terminal;
        CHECK(_wait_radio_terminal(operation, &terminal));
        CHECK(terminal.session_id == session);
        CHECK(terminal.operation_result == ESP_FAIL &&
              terminal.operation_failure == WIFI_SERVICE_FAILURE_DRIVER &&
              !terminal.operation_canceled && terminal.ipv4_address == 0U);
        CHECK(terminal.available == (mode >= 4U));
        CHECK(terminal.state == (mode >= 4U ? WIFI_SERVICE_STATE_IDLE :
                                 WIFI_SERVICE_STATE_OFFLINE));
        CHECK(wifi_service_test_credentials_are_zero());
        CHECK(wifi_service_deinit(WIFI_SERVICE_WAIT_FOREVER) == ESP_OK);
        CHECK(host_wifi_port_credentials_are_zero());
        printf("radio transition mode=%u operation=%llu failed=%d available=%u\n",
               mode, (unsigned long long)operation, terminal.operation_result,
               terminal.available);
    }
    return true;
}

static bool _test_radio_suspend_releases_without_terminal(void)
{
    host_wifi_port_reset();
    const wifi_service_config_t config = { .task_priority = 4U };
    const wifi_service_credentials_t credentials =
    {
        .ssid = "Suspend AP", .ssid_length = 10U,
        .password = "password1", .password_length = 9U,
        .security = WIFI_SERVICE_SECURITY_PERSONAL,
    };
    CHECK(wifi_service_init(&config) == ESP_OK);
    wifi_service_session_id_t session = 0U;
    wifi_service_operation_id_t operation = 0U;
    CHECK(wifi_service_session_open(&session) == ESP_OK);
    CHECK(wifi_service_request_connect(session, &credentials, &operation) == ESP_OK);
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    host_wifi_port_fail_next(HOST_WIFI_PORT_STOP, 1U, ESP_FAIL);
    host_wifi_port_leave_stopped_after_next_stop_failure();
    CHECK(wifi_service_suspend(TEST_TIMEOUT_MS) == ESP_FAIL);
    wifi_service_status_snapshot_t suspended;
    CHECK(wifi_service_get_status(&suspended) == ESP_OK);
    CHECK(suspended.state == WIFI_SERVICE_STATE_SUSPENDED &&
          suspended.ipv4_address == 0U && !suspended.operation_complete);
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(host_wifi_port_credentials_are_zero());
    CHECK(wifi_service_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_IDLE));
    CHECK(wifi_service_request_connect(session, &credentials, &operation) == ESP_OK);
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    wifi_service_cancel_disposition_t disposition;
    CHECK(wifi_service_cancel(session, operation, &disposition) == ESP_OK);
    CHECK(disposition == WIFI_SERVICE_CANCEL_ACCEPTED);
    wifi_service_status_snapshot_t terminal;
    CHECK(_wait_radio_terminal(operation, &terminal));
    CHECK(terminal.operation_canceled && terminal.operation_result == ESP_ERR_INVALID_STATE &&
          terminal.operation_failure == WIFI_SERVICE_FAILURE_NONE);
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(host_wifi_port_credentials_are_zero());
    CHECK(wifi_service_deinit(WIFI_SERVICE_WAIT_FOREVER) == ESP_OK);
    puts("radio suspend: physical SUSPENDED retains original error, release-only; cancel erases credentials");
    return true;
}

static bool _wait_ipv4(uint32_t address, wifi_service_state_t radio_state)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t radio;
        connectivity_manager_status_snapshot_t manager;
        if (wifi_service_get_status(&radio) == ESP_OK &&
                connectivity_manager_get_status(&manager) == ESP_OK &&
                radio.ipv4_address == address && manager.ipv4_address == address &&
                radio.state == radio_state)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _test_ipv4_renewal_preserves_operation(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_operation_id_t connect = 0U;
    CHECK(_request_personal_connect("Renew AP", "password1", &connect));
    CHECK(_complete_connection(UINT32_C(0x01020304)));
    CHECK(_wait_terminal(connect, ESP_OK, false));
    const unsigned writes = host_nv_storage_set_count();
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_GOT_IP,
                        WIFI_SERVICE_FAILURE_NONE, UINT32_C(0x05060708)) == ESP_OK);
    CHECK(_wait_ipv4(UINT32_C(0x05060708), WIFI_SERVICE_STATE_IP_READY));
    CHECK(_terminal_observer_count(connect, ESP_OK, false) == 1U);
    CHECK(host_nv_storage_set_count() == writes);
    _set_scan_record("Renew AP", WIFI_SERVICE_SECURITY_PERSONAL);
    connectivity_manager_operation_id_t scan = 0U;
    CHECK(connectivity_manager_request_scan(&scan) == ESP_OK);
    CHECK(_wait_scan_operation(scan, true));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_GOT_IP,
                        WIFI_SERVICE_FAILURE_NONE, UINT32_C(0x090A0B0C)) == ESP_OK);
    CHECK(_wait_ipv4(UINT32_C(0x090A0B0C), WIFI_SERVICE_STATE_SCANNING));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    CHECK(_wait_terminal(scan, ESP_OK, true));
    CHECK(_wait_ipv4(UINT32_C(0x090A0B0C), WIFI_SERVICE_STATE_IP_READY));
    CHECK(_terminal_observer_count(connect, ESP_OK, false) == 1U);
    CHECK(host_nv_storage_set_count() == writes);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("IPv4 renewal: connected and scanning update IP without new terminal/profile write");
    return true;
}

static bool _test_candidate_restart_failure_preserves_profile(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_operation_id_t first = 0U;
    CHECK(_request_personal_connect("Durable A", "password1", &first));
    CHECK(_complete_connection(UINT32_C(0x01020304)));
    CHECK(_wait_terminal(first, ESP_OK, false));
    uint8_t saved[TEST_PROFILE_BYTES];
    size_t saved_size = 0U;
    CHECK(host_nv_storage_copy(saved, sizeof(saved), &saved_size));
    const unsigned writes = host_nv_storage_set_count();
    host_wifi_port_fail_next(HOST_WIFI_PORT_STOP, 1U, ESP_FAIL);
    connectivity_manager_operation_id_t second = 0U;
    CHECK(_request_personal_connect("Candidate B", "password2", &second));
    CHECK(_wait_terminal(second, ESP_FAIL, false));
    CHECK(_terminal_observer_count(second, ESP_OK, false) == 0U);
    CHECK(_terminal_observer_count(second, ESP_FAIL, false) == 1U);
    CHECK(host_nv_storage_set_count() == writes);
    uint8_t retained[TEST_PROFILE_BYTES];
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == saved_size && memcmp(retained, saved, saved_size) == 0);
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("candidate restart failure: unique failure, durable A unchanged, no false B success");
    return true;
}

/* Exercise the public manager contract, retaining only hardware-boundary IDs. */
static bool _test_candidate_suspend_resume(bool sync, bool waiting_ip,
        bool stop_error, bool resume_error)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    host_wifi_port_set_clear_requires_stopped(true);
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = "Parked candidate", .ssid_length = 16U,
        .password = "password1", .password_length = 9U,
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    connectivity_manager_operation_id_t operation = 0U;
    if (sync)
    {
        CHECK(connectivity_manager_request_sync_profile(&credentials,
                UINT64_C(0x987654), false, &operation) == ESP_OK);
    }
    else
    {
        CHECK(connectivity_manager_request_connect(&credentials, &operation) == ESP_OK);
    }
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    if (waiting_ip)
    {
        CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_CONNECTED,
                            WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_WAITING_IP, NULL));
    }
    wifi_service_status_snapshot_t old_radio;
    CHECK(wifi_service_get_status(&old_radio) == ESP_OK);
    const uint64_t old_epoch = host_wifi_port_epoch();
    const unsigned connects = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    if (stop_error)
    {
        host_wifi_port_fail_next(HOST_WIFI_PORT_STOP, 1U, ESP_FAIL);
        host_wifi_port_leave_stopped_after_next_stop_failure();
    }
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) ==
          (stop_error ? ESP_FAIL : ESP_OK));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(wifi_service_test_credentials_are_zero());
    CHECK(host_wifi_port_credentials_are_zero());
    CHECK(host_nv_storage_set_count() == 0U);
    CHECK(_terminal_observer_count(operation, ESP_OK, false) == 0U);
    if (resume_error)
    {
        host_wifi_port_fail_next(HOST_WIFI_PORT_START, 1U, ESP_FAIL);
        CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_FAIL);
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
        CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
        CHECK(host_nv_storage_set_count() == 0U);
    }
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_driver_ssid("Parked candidate"));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT, connects + 1U,
                                    TEST_TIMEOUT_MS));
    wifi_service_status_snapshot_t new_radio;
    CHECK(_wait_radio_connecting_with_new_operation(old_radio.operation_id,
            &new_radio));
    CHECK(host_wifi_port_epoch() != old_epoch);
    CHECK(_wait_status_operation(operation, false));
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    const wifi_service_port_event_t stale =
    {
        .type = WIFI_SERVICE_PORT_EVENT_GOT_IP, .epoch = old_epoch,
        .status = ESP_OK, .ipv4_address = UINT32_C(0x01020304),
    };
    CHECK(wifi_service_port_submit_event(&stale) == ESP_OK);
    /* A current association event is a FIFO barrier behind the stale IP. */
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_CONNECTED,
                        WIFI_SERVICE_FAILURE_NONE, 0U) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_WAITING_IP, NULL));
    CHECK(_wait_status_operation(operation, false));
    CHECK(host_nv_storage_set_count() == 0U);
    CHECK(_terminal_observer_count(operation, ESP_OK, false) == 0U);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_GOT_IP,
                        WIFI_SERVICE_FAILURE_NONE, UINT32_C(0x05060708)) == ESP_OK);
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_terminal_observer_count(operation, ESP_OK, false) == 1U);
    CHECK(host_nv_storage_set_count() == 1U);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects + 1U);
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.saved_profile && strcmp(status.ssid, "Parked candidate") == 0);
    if (sync)
    {
        CHECK(status.applied_client_sync_id == UINT64_C(0x987654));
        CHECK(!status.auto_connect);
        connectivity_manager_operation_id_t repeat = 0U;
        CHECK(connectivity_manager_request_sync_profile(&credentials,
                UINT64_C(0x987654), false, &repeat) == ESP_OK);
        CHECK(_wait_terminal(repeat, ESP_OK, false));
        CHECK(host_nv_storage_set_count() == 1U);
        CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects + 1U);
    }
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    if (sync)
    {
        CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
        CHECK(status.saved_profile && !status.auto_connect);
        CHECK(status.applied_client_sync_id == UINT64_C(0x987654));
        connectivity_manager_operation_id_t persisted_repeat = 0U;
        CHECK(connectivity_manager_request_sync_profile(&credentials,
                UINT64_C(0x987654), false, &persisted_repeat) == ESP_OK);
        CHECK(_wait_terminal(persisted_repeat, ESP_OK, false));
        CHECK(host_nv_storage_set_count() == 1U);
        CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects + 1U);
        CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
        CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
        CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects + 1U);
        CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    }
    printf("candidate recovery: %s/%s id=%llu radio=%llu->%llu stop_error=%u resume_error=%u unique success, one commit\n",
           sync ? "SYNC" : "CONNECT", waiting_ip ? "WAITING_IP" : "CONNECTING",
           (unsigned long long)operation, (unsigned long long)old_radio.operation_id,
           (unsigned long long)new_radio.operation_id, stop_error, resume_error);
    return true;
}

static bool _test_suspended_candidate_preemption(bool cancel, bool forget)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    connectivity_manager_operation_id_t first = 0U;
    CHECK(_request_personal_connect("Parked A", "password1", &first));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
    const unsigned connects = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    connectivity_manager_operation_id_t replacement = 0U;
    if (cancel)
    {
        CHECK(connectivity_manager_cancel(first) == ESP_OK);
    }
    else if (forget)
    {
        CHECK(connectivity_manager_request_forget(&replacement) == ESP_OK);
    }
    else
    {
        CHECK(_request_personal_connect("Replacement B", "password2", &replacement));
    }
    CHECK(_wait_terminal(first, ESP_ERR_NOT_FINISHED, false));
    CHECK(_terminal_observer_count(first, ESP_ERR_NOT_FINISHED, false) == 1U);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_SUSPENDED, NULL));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
    CHECK(host_nv_storage_set_count() == 0U);
    CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
    if (!cancel && !forget)
    {
        CHECK(_wait_driver_ssid("Replacement B"));
        CHECK(_complete_connection(UINT32_C(0x09080706)));
        CHECK(_wait_terminal(replacement, ESP_OK, false));
        CHECK(_terminal_observer_count(replacement, ESP_OK, false) == 1U);
        CHECK(host_nv_storage_set_count() == 1U);
    }
    else
    {
        if (forget)
        {
            CHECK(_wait_terminal(replacement, ESP_ERR_NOT_FOUND, false));
        }
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
        CHECK(connectivity_manager_suspend(TEST_TIMEOUT_MS) == ESP_OK);
        CHECK(connectivity_manager_resume(TEST_TIMEOUT_MS) == ESP_OK);
        CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
        CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
        CHECK(_terminal_observer_count(first, ESP_OK, false) == 0U);
    }
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    printf("suspended candidate: id=%llu %s, unique local cancellation, no retired attempt revival\n",
           (unsigned long long)first, cancel ? "cancel" : forget ? "forget" : "replace");
    return true;
}

/* Save one profile without connecting so several entries can accumulate. */
static bool _save_profile(const char *ssid, const char *password,
                          connectivity_manager_operation_id_t *operation_id)
{
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = ssid,
        .ssid_length = strlen(ssid),
        .password = password,
        .password_length = strlen(password),
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    return connectivity_manager_request_save_profile(
               &credentials, operation_id) == ESP_OK;
}

static bool _wait_profile_count(uint8_t expected,
                                connectivity_manager_profile_snapshot_t *out)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_profile_snapshot_t snapshot;

        if (connectivity_manager_get_profiles(&snapshot) == ESP_OK &&
                snapshot.count == expected)
        {
            if (out != NULL)
            {
                *out = snapshot;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

/* Multiple saved networks: upsert, capacity, targeted removal, and the
 * password-free snapshot the UI reads. */
static bool _test_profile_list(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_profile_snapshot_t profiles;
    CHECK(_wait_profile_count(0U, &profiles));
    CHECK(profiles.active_index == UINT8_MAX);

    connectivity_manager_operation_id_t operation = 0U;
    CHECK(_save_profile("Alpha", "alphapass", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_wait_profile_count(1U, &profiles));
    CHECK(strcmp(profiles.profiles[0].ssid, "Alpha") == 0);
    CHECK(profiles.profiles[0].active == 1U);
    CHECK(profiles.active_index == 0U);
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.saved_profile);
    CHECK(status.profile_count == 1U);
    CHECK(strcmp(status.ssid, "Alpha") == 0);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == 0U);

    /* The same SSID replaces the entry instead of appending a duplicate. */
    CHECK(_save_profile("Alpha", "newalphapass", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_wait_profile_count(1U, &profiles));
    CHECK(strcmp(profiles.profiles[0].ssid, "Alpha") == 0);

    CHECK(_save_profile("Bravo", "bravopass1", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_wait_profile_count(2U, &profiles));
    /* Saving a network selects it, like the contract's SET_CREDENTIALS. */
    CHECK(profiles.profiles[0].active == 0U);
    CHECK(profiles.profiles[1].active == 1U);
    CHECK(profiles.active_index == 1U);
    CHECK(profiles.order[0] == 1U);
    CHECK(profiles.order[1] == 0U);

    CHECK(_save_profile("Charlie", "charliepw1", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Delta", "deltapass1", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_wait_profile_count(4U, &profiles));

    /* A fifth distinct network is refused instead of silently evicting one. */
    CHECK(_save_profile("Echo", "echopass12", &operation));
    CHECK(_wait_terminal(operation, ESP_ERR_NO_MEM, false));
    CHECK(_wait_profile_count(4U, &profiles));

    /* Targeted connect selects the named network, not the active one. */
    connectivity_manager_operation_id_t connect = 0U;
    CHECK(connectivity_manager_request_connect_network("Bravo",
            strlen("Bravo"), &connect) == ESP_OK);
    CHECK(_wait_driver_ssid("Bravo"));
    CHECK(_complete_connection(UINT32_C(0x0a0b0c0d)));
    CHECK(_wait_terminal(connect, ESP_OK, false));
    CHECK(_wait_profile_count(4U, &profiles));
    CHECK(profiles.profiles[0].active == 0U);
    CHECK(profiles.profiles[1].active == 1U);
    CHECK(profiles.active_index == 1U);
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(strcmp(status.ssid, "Bravo") == 0);
    const unsigned writes = host_nv_storage_set_count();

    /* An unknown SSID completes with NOT_FOUND and leaves the list intact. */
    connectivity_manager_operation_id_t missing = 0U;
    CHECK(connectivity_manager_request_connect_network("Missing",
            strlen("Missing"), &missing) == ESP_OK);
    CHECK(_wait_terminal(missing, ESP_ERR_NOT_FOUND, false));
    CHECK(host_nv_storage_set_count() == writes);
    CHECK(_wait_profile_count(4U, &profiles));

    /* Removing an inactive entry must not disturb the link or its profile. */
    const unsigned connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget_network("Delta",
            strlen("Delta"), &forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_OK, false));
    CHECK(_wait_profile_count(3U, &profiles));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.state == CONNECTIVITY_MANAGER_STATE_IP_READY);
    CHECK(strcmp(status.ssid, "Bravo") == 0);
    CHECK(profiles.active_index == 1U);
    CHECK(profiles.profiles[1].active == 1U);

    /* Removing the active entry disconnects and keeps the remaining entries. */
    CHECK(connectivity_manager_request_forget_network("Bravo",
            strlen("Bravo"), &forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_OK, false));
    CHECK(_wait_profile_count(2U, &profiles));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.state == CONNECTIVITY_MANAGER_STATE_IDLE);
    CHECK(status.saved_profile);
    CHECK(status.profile_count == 2U);
    for (size_t index = 0U; index < profiles.count; ++index)
    {
        CHECK(strcmp(profiles.profiles[index].ssid, "Bravo") != 0);
        CHECK(strcmp(profiles.profiles[index].ssid, "Delta") != 0);
    }
    CHECK(wifi_service_test_credentials_are_zero());

    /* The list survives a full manager restart unchanged. */
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_profile_count(2U, &profiles));
    CHECK(strcmp(profiles.profiles[0].ssid, "Alpha") == 0);
    CHECK(strcmp(profiles.profiles[1].ssid, "Charlie") == 0);

    /* FORGET clears every entry, which is what the contract's profile_ssid
       absence depends on. */
    connectivity_manager_operation_id_t forget_all = 0U;
    CHECK(connectivity_manager_request_forget(&forget_all) == ESP_OK);
    CHECK(_wait_terminal(forget_all, ESP_OK, false));
    CHECK(_wait_profile_count(0U, &profiles));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.saved_profile);
    CHECK(status.profile_count == 0U);
    CHECK(status.ssid[0] == '\0');
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("profile list: upsert, capacity, targeted removal, restart, forget-all");
    return true;
}

/* Two saved networks: a failed candidate advances to the next one instead of
 * consuming the executor's internal retries. */
static bool _test_profile_sweep_fallback(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t operation = 0U;
    CHECK(_save_profile("Sweep A", "sweepapass", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Sweep B", "sweepbpass", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));

    connectivity_manager_profile_snapshot_t profiles;
    CHECK(_wait_profile_count(2U, &profiles));
    CHECK(strcmp(profiles.profiles[1].ssid, "Sweep B") == 0);
    CHECK(profiles.profiles[1].active == 1U);

    /* Reconnect starts on the selected entry, which is missing. */
    connectivity_manager_operation_id_t reconnect = 0U;
    CHECK(connectivity_manager_request_reconnect_saved(&reconnect) == ESP_OK);
    CHECK(_wait_driver_ssid("Sweep B"));
    const unsigned first_attempt =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AP_NOT_FOUND, 0U) == ESP_OK);
    /* The next attempt must be the other saved network, immediately. */
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    first_attempt + 1U, TEST_TIMEOUT_MS));
    CHECK(_wait_driver_ssid("Sweep A"));
    CHECK(_complete_connection(UINT32_C(0x0a01a8c0)));
    CHECK(_wait_terminal(reconnect, ESP_OK, false));
    CHECK(_wait_profile_count(2U, &profiles));
    CHECK(strcmp(profiles.profiles[0].ssid, "Sweep A") == 0);
    CHECK(profiles.profiles[0].active == 1U);
    CHECK(strcmp(profiles.profiles[1].ssid, "Sweep B") == 0);
    CHECK(profiles.profiles[1].active == 0U);
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.state == CONNECTIVITY_MANAGER_STATE_IP_READY);
    CHECK(strcmp(status.ssid, "Sweep A") == 0);

    /* Every saved network failing surfaces the last classified failure. Sweep
     * A is now selected, so B is the single fallback. */
    connectivity_manager_operation_id_t failing = 0U;
    unsigned attempts = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_request_reconnect_saved(&failing) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    attempts + 1U, TEST_TIMEOUT_MS));
    CHECK(_wait_driver_ssid("Sweep A"));
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    attempts = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AP_NOT_FOUND, 0U) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                    attempts + 1U, TEST_TIMEOUT_MS));
    CHECK(_wait_driver_ssid("Sweep B"));
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    attempts = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AP_NOT_FOUND, 0U) == ESP_OK);
    connectivity_manager_status_snapshot_t retry;
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_RETRY_WAIT,
                               CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND,
                               &retry));
    /* An exhausted sweep parks in the long-backoff policy instead of publishing
     * a failure terminal; cancellation ends the operation. */
    CHECK(!retry.operation_complete);
    CHECK(retry.retry_count == 1U);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == attempts);
    CHECK(_terminal_observer_count(failing, ESP_FAIL, false) == 0U);
    CHECK(connectivity_manager_cancel(failing) == ESP_OK);
    CHECK(_wait_terminal(failing, ESP_ERR_NOT_FINISHED, false));
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    CHECK(wifi_service_test_credentials_are_zero());
    puts("profile sweep: next candidate on AP_NOT_FOUND, exhausted sweep backs off");
    return true;
}

/* Build a v4 record wider than the configured limit so the downgrade path can
 * be exercised; the manager must keep the list usable instead of writing back a
 * record it can no longer load. */
static void _build_wide_record(uint8_t *record, uint8_t count,
                               uint8_t active_index)
{
    memset(record, 0, TEST_PROFILE_BYTES);
    const uint32_t magic = UINT32_C(0x57465031);
    const uint16_t version = 4U;
    const uint64_t revision = 3U;

    memcpy(record, &magic, sizeof(magic));
    memcpy(record + PROFILE_VERSION_OFFSET, &version, sizeof(version));
    record[PROFILE_COUNT_OFFSET] = count;
    record[PROFILE_ACTIVE_OFFSET] = active_index;
    /* Leave the automatic-connect policy off so this case observes loading and
     * the rewrite instead of the radio it would start. */
    record[PROFILE_AUTO_CONNECT_OFFSET] = 0U;
    memcpy(record + PROFILE_REVISION_OFFSET, &revision, sizeof(revision));
    for (size_t index = 0U; index < count; ++index)
    {
        uint8_t *entry = record + PROFILE_ENTRIES_OFFSET +
                         index * PROFILE_ENTRY_BYTES;
        /* Exactly eight printable bytes: an SSID is a byte string, so a shorter
         * literal would embed a NUL inside the declared length and be rejected. */
        const char ssid[8] =
        {
            'W', 'i', 'd', 'e', '0', '0', (char)('0' + (index % 10U)),
            (char)('a' + (index % 26U)),
        };

        entry[0] = 1U;                    /* PERSONAL */
        entry[1] = 8U;                    /* ssid_length */
        entry[2] = 8U;                    /* password_length */
        memcpy(entry + 4U, ssid, 8U);
        memcpy(entry + 36U, "widepw12", 8U);
    }
}

/* A record written with more slots than this build keeps must stay loadable
 * after the manager rewrites it, and the selected index must address a retained
 * entry. */
static bool _test_profile_downgrade_load(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    uint8_t record[TEST_PROFILE_BYTES];

    _build_wide_record(record, UINT8_C(5), UINT8_C(4));
    host_nv_storage_seed(record, PROFILE_RECORD_BYTES);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_profile_snapshot_t profiles;
    CHECK(_wait_profile_count((uint8_t)CONNECTIVITY_MANAGER_MAX_PROFILES,
                              &profiles));
    CHECK(profiles.active_index < profiles.count);
    CHECK(profiles.profiles[profiles.active_index].active == 1U);
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.saved_profile);
    CHECK(status.profile_count == (uint8_t)CONNECTIVITY_MANAGER_MAX_PROFILES);
    CHECK(status.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);

    /* The clamping rewrite must have aligned flash with RAM, so the record the
     * manager itself wrote is loadable again. */
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));
    CHECK(_wait_profile_count((uint8_t)CONNECTIVITY_MANAGER_MAX_PROFILES,
                              &profiles));
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.saved_profile);
    CHECK(status.profile_count == (uint8_t)CONNECTIVITY_MANAGER_MAX_PROFILES);
    CHECK(status.failure == CONNECTIVITY_MANAGER_FAILURE_NONE);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("profile downgrade: wider record clamped, selected index stays valid");
    return true;
}

/* An exhausted sweep must keep the user's selection instead of leaving the last
 * network it tried as the selected profile. */
static bool _test_profile_sweep_keeps_selection(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t operation = 0U;
    CHECK(_save_profile("Keep A", "keepa123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Keep B", "keepb123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Keep C", "keepc123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));

    connectivity_manager_profile_snapshot_t profiles;
    CHECK(_wait_profile_count(3U, &profiles));
    CHECK(profiles.active_index == 2U);
    const uint8_t selected = profiles.active_index;

    connectivity_manager_operation_id_t reconnect = 0U;
    unsigned attempts = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    CHECK(connectivity_manager_request_reconnect_saved(&reconnect) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT, attempts + 1U,
                                    TEST_TIMEOUT_MS));
    CHECK(_wait_driver_ssid("Keep C"));
    CHECK(_wait_wifi_service_status(WIFI_SERVICE_STATE_CONNECTING));
    /* Both fallbacks fail; the third failure ends the sweep. */
    for (unsigned step = 0U; step < 3U; ++step)
    {
        CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                            WIFI_SERVICE_FAILURE_AP_NOT_FOUND, 0U) == ESP_OK);
        if (step < 2U)
        {
            attempts = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
            CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT,
                                            attempts + 1U,
                                            TEST_TIMEOUT_MS));
            CHECK(_wait_wifi_service_status(
                      WIFI_SERVICE_STATE_CONNECTING));
        }
    }
    connectivity_manager_status_snapshot_t retry;
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_RETRY_WAIT,
                               CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND,
                               &retry));
    /* The selected profile is still the user's choice, and the projected SSID
     * agrees with it rather than naming the last network tried. */
    CHECK(_wait_profile_count(3U, &profiles));
    CHECK(profiles.active_index == selected);
    CHECK(profiles.profiles[selected].active == 1U);
    CHECK(strcmp(retry.ssid, "Keep C") == 0);

    /* A durable write must not persist a drifted selection either. */
    connectivity_manager_operation_id_t auto_op = 0U;
    CHECK(connectivity_manager_set_auto_connect(false, &auto_op) == ESP_OK);
    CHECK(_wait_terminal(auto_op, ESP_OK, false));
    uint8_t record[TEST_PROFILE_BYTES];
    size_t record_size = 0U;
    CHECK(host_nv_storage_copy(record, sizeof(record), &record_size));
    CHECK(record_size == PROFILE_RECORD_BYTES);
    CHECK(record[PROFILE_ACTIVE_OFFSET] == selected);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("profile sweep: exhausted sweep keeps the selected profile");
    return true;
}

/* Removing an entry that does not carry the link must not switch the radio into
 * the manual-offline hold. */
static bool _test_forget_inactive_keeps_policy(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t operation = 0U;
    CHECK(_save_profile("Hold A", "holda123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Hold B", "holdb123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Hold C", "holdc123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));

    connectivity_manager_operation_id_t connect = 0U;
    CHECK(connectivity_manager_request_connect_network("Hold C",
            strlen("Hold C"), &connect) == ESP_OK);
    CHECK(_wait_driver_ssid("Hold C"));
    CHECK(_complete_connection(UINT32_C(0x0a0b0c0d)));
    CHECK(_wait_terminal(connect, ESP_OK, false));

    const unsigned connects =
        host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget_network("Hold B",
            strlen("Hold B"), &forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_OK, false));
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.state == CONNECTIVITY_MANAGER_STATE_IP_READY);
    CHECK(!status.manual_hold);
    CHECK(status.ipv4_address == UINT32_C(0x0a0b0c0d));
    CHECK(status.auto_connect);
    CHECK(status.profile_count == 2U);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connects);

    /* The automatic-connect policy is still in force, so a link loss schedules a
     * retry instead of parking the radio offline. */
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_LINK_LOST, 0U) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_RETRY_WAIT,
                               CONNECTIVITY_MANAGER_FAILURE_LINK_LOST,
                               &status));
    CHECK(!status.manual_hold);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("profile forget: inactive removal keeps the link and the policy");
    return true;
}

/* A refused removal write must not leave the RAM list advertising a deletion
 * that never reached flash. */
static bool _test_forget_store_failure_rolls_back(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    _terminal_observer_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t operation = 0U;
    CHECK(_save_profile("Roll A", "rolla123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    CHECK(_save_profile("Roll B", "rollb123", &operation));
    CHECK(_wait_terminal(operation, ESP_OK, false));
    connectivity_manager_profile_snapshot_t profiles;
    CHECK(_wait_profile_count(2U, &profiles));

    uint8_t before[TEST_PROFILE_BYTES];
    size_t before_size = 0U;
    CHECK(host_nv_storage_copy(before, sizeof(before), &before_size));

    host_nv_storage_fail_next_set(ESP_FAIL);
    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget_network("Roll A",
            strlen("Roll A"), &forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_FAIL, false));

    /* RAM and flash agree: both still hold the entry the removal tried to drop. */
    CHECK(_wait_profile_count(2U, &profiles));
    uint8_t after[TEST_PROFILE_BYTES];
    size_t after_size = 0U;
    CHECK(host_nv_storage_copy(after, sizeof(after), &after_size));
    CHECK(after_size == before_size);
    CHECK(memcmp(before, after, before_size) == 0);
    CHECK(strcmp(profiles.profiles[0].ssid, "Roll A") == 0);
    CHECK(strcmp(profiles.profiles[1].ssid, "Roll B") == 0);

    /* A reboot observes the same list, so the rolled-back removal is invisible.
     * The record keeps automatic connection enabled, so the boot may start an
     * attempt; only the list contents matter here. */
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    host_wifi_port_reset();
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_profile_count(2U, &profiles));
    CHECK(strcmp(profiles.profiles[0].ssid, "Roll A") == 0);
    CHECK(strcmp(profiles.profiles[1].ssid, "Roll B") == 0);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    puts("profile forget: refused write rolls the removal back");
    return true;
}

static bool _run_pipeline(void)
{
    host_freertos_reset_controls();
    host_wifi_port_reset();
    host_nv_storage_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(app_manager_get_ui_dispatch_fn(&s_real_ui_dispatch) == ESP_OK);
    CHECK(s_real_ui_dispatch != NULL);
    CHECK(event_bus_register_ui_dispatch(_test_ui_dispatch) == ESP_OK);
    event_bus_sub_handle_t status_terminal_subscription =
        EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t scan_terminal_subscription =
        EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t scan_ui_subscription =
        EVENT_BUS_SUB_HANDLE_INVALID;
    CHECK(event_bus_subscribe(
              CONNECTIVITY_MANAGER_MSG,
              CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
              _terminal_status_event, NULL, EVENT_BUS_DISPATCH_PUBLISHER,
              &status_terminal_subscription) == ESP_OK);
    CHECK(event_bus_subscribe(
              CONNECTIVITY_MANAGER_MSG,
              CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
              _terminal_scan_event, NULL, EVENT_BUS_DISPATCH_PUBLISHER,
              &scan_terminal_subscription) == ESP_OK);
    CHECK(event_bus_subscribe(
              CONNECTIVITY_MANAGER_MSG,
              CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
              _scan_ui_event, NULL, EVENT_BUS_DISPATCH_UI,
              &scan_ui_subscription) == ESP_OK);
    CHECK(_run_on_ui(_ui_capture_worker, NULL));

    CHECK(_test_wifi_cancel_dispositions());
    CHECK(_test_radio_transition_failures());
    CHECK(_test_radio_suspend_releases_without_terminal());
    CHECK(_test_ipv4_renewal_preserves_operation());
    CHECK(_test_candidate_restart_failure_preserves_profile());
    CHECK(_test_pending_sync_profile(false, false));
    CHECK(_test_pending_sync_profile(true, false));
    CHECK(_test_pending_sync_profile(false, true));
    CHECK(_test_candidate_suspend_resume(false, false, false, false));
    CHECK(_test_candidate_suspend_resume(false, true, false, false));
    CHECK(_test_candidate_suspend_resume(true, false, false, false));
    CHECK(_test_candidate_suspend_resume(true, true, false, false));
    CHECK(_test_candidate_suspend_resume(false, false, true, false));
    CHECK(_test_candidate_suspend_resume(true, true, false, true));
    CHECK(_test_suspended_candidate_preemption(true, false));
    CHECK(_test_suspended_candidate_preemption(false, false));
    CHECK(_test_suspended_candidate_preemption(false, true));
    CHECK(_test_clear_persisted_profile_lifecycle());
    CHECK(_test_manager_init_resource_failures());
    CHECK(_test_operation_arbitration());
    CHECK(_test_candidate_terminal_cleanup());
    CHECK(_test_cancel_failure_terminal());
    CHECK(_test_save_profile_does_not_connect());
    CHECK(_test_missing_profile_terminals());
    CHECK(_test_profile_read_failure_classification());
    CHECK(_test_hex_psk_recovery());
    CHECK(_test_profile_list());
    CHECK(_test_profile_sweep_fallback());
    CHECK(_test_profile_downgrade_load());
    CHECK(_test_profile_sweep_keeps_selection());
    CHECK(_test_forget_inactive_keeps_policy());
    CHECK(_test_forget_store_failure_rolls_back());
    uint8_t saved_record[TEST_PROFILE_BYTES];
    size_t saved_size = 0U;
    CHECK(_test_foreground_and_persistence(saved_record, &saved_size));
    CHECK(_test_sync_window_restart(saved_record, saved_size));
    CHECK(_test_scan_resume_guards(saved_record, saved_size));
    CHECK(_test_scan_suspend_resume(saved_record, saved_size));
    CHECK(_test_boot_retry_timeouts(saved_record, saved_size));
    CHECK(_test_active_radio_failure_terminal(saved_record, saved_size));
    CHECK(_test_invalid_profile(saved_record, saved_size));
    CHECK(_test_legacy_profile_migration(saved_record, saved_size));
    CHECK(_test_manager_deinit_terminal());
    CHECK(_test_terminal_outbox_deinit_barrier());
    CHECK(_test_manager_deinit_retry());
    CHECK(_test_queue_overflow());
    CHECK(_test_control_completion_generation());

    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(!host_wifi_port_thread_violation());
    CHECK(!host_wifi_port_scan_ownership_violation());
    CHECK(event_bus_unsubscribe(scan_ui_subscription) == ESP_OK);
    CHECK(event_bus_unsubscribe(scan_terminal_subscription) == ESP_OK);
    CHECK(event_bus_unsubscribe(status_terminal_subscription) == ESP_OK);
    CHECK(event_bus_unregister_ui_dispatch(_test_ui_dispatch) == ESP_OK);
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    return true;
}

int main(void)
{
    if (!_run_pipeline())
    {
        return 1;
    }
    puts("production connectivity manager pipeline tests passed");
    return 0;
}

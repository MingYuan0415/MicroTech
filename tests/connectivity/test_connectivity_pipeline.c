#include "app_manager_mailbox.h"
#include "connectivity_manager.h"
#include "event_bus.h"
#include "freertos/task.h"
#include "host_freertos.h"
#include "host_nv_storage.h"
#include "host_wifi_port.h"
#include "setup_wifi_adapter.h"
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
static atomic_bool s_block_ui_dispatch = ATOMIC_VAR_INIT(false);
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
    esp_err_t result, bool scan)
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
                                      snapshot->last_error, false);
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
                                      snapshot->last_error, true);
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
    CHECK(_wait_scan(false, ESP_ERR_NOT_FINISHED, NULL));
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
    CHECK(host_nv_storage_copy(saved_record, 256U, saved_size));
    CHECK(*saved_size > 0U);
    CHECK(_run_on_ui(_ui_barrier, NULL));

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

    const unsigned disconnect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_DISCONNECT);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_LINK_LOST, 0U) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_DISCONNECT,
                                    disconnect_before + 1U,
                                    TEST_TIMEOUT_MS));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IDLE, &status));
    CHECK(!status.auto_connect);

    CHECK(_run_on_ui(_ui_auto_on, &adapter));
    CHECK(_wait_auto_connect(true));
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    CHECK(_complete_connection(UINT32_C(0x0302a8c0)));
    CHECK(_run_on_ui(_ui_barrier, NULL));

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
    uint8_t retained[256];
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == *saved_size);
    CHECK(memcmp(retained, saved_record, retained_size) == 0);

    const unsigned candidate_disconnect_before =
        host_wifi_port_call_count(HOST_WIFI_PORT_DISCONNECT);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_LINK_LOST, 0U) == ESP_OK);
    CHECK(host_wifi_port_wait_calls(HOST_WIFI_PORT_DISCONNECT,
                                    candidate_disconnect_before + 1U,
                                    TEST_TIMEOUT_MS));
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
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_IP_READY, &status));
    CHECK(status.saved_profile);
    CHECK(!status.manual_hold);
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
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
                        WIFI_SERVICE_FAILURE_AUTHENTICATION, 0U) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION,
                               NULL));

    connectivity_manager_operation_id_t reconnect = 0U;
    CHECK(connectivity_manager_request_reconnect_saved(&reconnect) == ESP_OK);
    CHECK(_wait_status(CONNECTIVITY_MANAGER_STATE_CONNECTING, NULL));
    host_wifi_port_fail_next(HOST_WIFI_PORT_START, 1U, ESP_FAIL);
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
                               CONNECTIVITY_MANAGER_FAILURE_STORAGE,
                               &status));
    CHECK(!status.saved_profile);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == connect_before);
    CHECK(host_nv_storage_erase_count() == 0U);
    CHECK(connectivity_manager_deinit(
              CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    uint8_t retained[256];
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(retained, sizeof(retained), &retained_size));
    CHECK(retained_size == size);
    CHECK(memcmp(retained, record, size) == 0);
    return true;
}

static bool _test_invalid_profile(const uint8_t saved_record[],
                                  size_t saved_size)
{
    connectivity_manager_status_snapshot_t status;
    enum
    {
        PROFILE_VERSION_OFFSET = 4U,
        PROFILE_RESERVED_OFFSET = 10U,
        PROFILE_SSID_OFFSET = 12U,
        PROFILE_PASSWORD_OFFSET = 44U,
        PROFILE_TRAILING_RESERVED_OFFSET = 108U,
    };
    uint8_t corrupted[256];
    CHECK(saved_size == 112U);
    CHECK(saved_size <= sizeof(corrupted));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_VERSION_OFFSET] ^= UINT8_C(0x7f);
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_SSID_OFFSET] = 0U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_PASSWORD_OFFSET] = 0U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_RESERVED_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    memcpy(corrupted, saved_record, saved_size);
    corrupted[PROFILE_TRAILING_RESERVED_OFFSET] = 1U;
    CHECK(_test_invalid_profile_record(corrupted, saved_size));

    host_nv_storage_seed(saved_record, saved_size - 1U);
    CHECK(connectivity_manager_init(&s_manager_config) == ESP_OK);
    CHECK(_wait_status_failure(CONNECTIVITY_MANAGER_STATE_IDLE,
                               CONNECTIVITY_MANAGER_FAILURE_STORAGE,
                               &status));
    CHECK(!status.saved_profile);
    size_t retained_size = 0U;
    CHECK(host_nv_storage_copy(NULL, 0U, &retained_size));
    CHECK(retained_size == saved_size - 1U);
    CHECK(host_nv_storage_erase_count() == 0U);
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

    CHECK(_test_clear_persisted_profile_lifecycle());
    CHECK(_test_manager_init_resource_failures());
    CHECK(_test_operation_arbitration());
    CHECK(_test_candidate_terminal_cleanup());
    uint8_t saved_record[256];
    size_t saved_size = 0U;
    CHECK(_test_foreground_and_persistence(saved_record, &saved_size));
    CHECK(_test_scan_resume_guards(saved_record, saved_size));
    CHECK(_test_scan_suspend_resume(saved_record, saved_size));
    CHECK(_test_boot_retry_timeouts(saved_record, saved_size));
    CHECK(_test_active_radio_failure_terminal(saved_record, saved_size));
    CHECK(_test_invalid_profile(saved_record, saved_size));
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

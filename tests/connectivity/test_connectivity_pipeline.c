#include "app_manager_mailbox.h"
#include "event_bus.h"
#include "freertos/task.h"
#include "host_freertos.h"
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
#define UI_TIMEOUT_MS   2000U
#define STATUS_STATE_COUNT ((size_t)WIFI_SERVICE_STATE_SUSPENDED + 1U)
#define SCAN_STATE_COUNT   ((size_t)WIFI_SERVICE_SCAN_FAILED + 1U)

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
    unsigned scan_count;
    unsigned status_states[STATUS_STATE_COUNT];
    unsigned scan_states[SCAN_STATE_COUNT];
    setup_wifi_status_scope_t last_scope;
    setup_wifi_operation_kind_t last_operation_kind;
    wifi_service_status_snapshot_t status;
    wifi_service_scan_snapshot_t scan;
} observer_t;

typedef struct open_args
{
    setup_wifi_adapter_t *adapter;
    observer_t *observer;
} open_args_t;

typedef struct adapter_query
{
    setup_wifi_adapter_t *adapter;
    wifi_service_session_id_t session_id;
    wifi_service_operation_id_t operation_id;
    setup_wifi_operation_kind_t operation_kind;
} adapter_query_t;

typedef struct connect_args
{
    setup_wifi_adapter_t *adapter;
    const char *ssid;
    size_t ssid_length;
    wifi_service_security_t security;
    uint8_t *password;
    size_t password_length;
} connect_args_t;

static TaskHandle_t s_ui_worker;
static atomic_uint s_noop_count;

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
    const wifi_service_status_snapshot_t *snapshot,
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

static void _observer_scan(const wifi_service_scan_snapshot_t *snapshot,
                           void *user_data)
{
    observer_t *observer = user_data;
    (void)pthread_mutex_lock(&observer->lock);
    if (xTaskGetCurrentTaskHandle() != s_ui_worker ||
            !app_manager_mailbox_is_worker())
    {
        observer->identity_violation = true;
    }
    ++observer->scan_count;
    if ((size_t)snapshot->state < SCAN_STATE_COUNT)
    {
        ++observer->scan_states[snapshot->state];
    }
    observer->scan = *snapshot;
    (void)pthread_mutex_unlock(&observer->lock);
}

static esp_err_t _ui_capture_worker(void *arg)
{
    (void)arg;
    s_ui_worker = xTaskGetCurrentTaskHandle();
    return s_ui_worker != NULL && app_manager_mailbox_is_worker() ?
           ESP_OK : ESP_FAIL;
}

static esp_err_t _ui_barrier(void *arg)
{
    (void)arg;
    return app_manager_mailbox_is_worker() ? ESP_OK : ESP_FAIL;
}

static esp_err_t _ui_open_adapter(void *arg)
{
    open_args_t *args = arg;
    const setup_wifi_adapter_callbacks_t callbacks =
    {
        .status = _observer_status,
        .scan = _observer_scan,
    };
    return setup_wifi_adapter_open(args->adapter, &callbacks, args->observer);
}

static esp_err_t _ui_close_adapter(void *arg)
{
    return setup_wifi_adapter_close(arg);
}

static esp_err_t _ui_scan(void *arg)
{
    return setup_wifi_adapter_scan(arg);
}

static esp_err_t _ui_connect(void *arg)
{
    connect_args_t *args = arg;
    return setup_wifi_adapter_connect(
               args->adapter, args->ssid, args->ssid_length,
               args->security, args->password, args->password_length);
}

static esp_err_t _ui_query_adapter(void *arg)
{
    adapter_query_t *query = arg;
    query->session_id = query->adapter->session_id;
    query->operation_id = query->adapter->operation_id;
    query->operation_kind = query->adapter->operation_kind;
    return ESP_OK;
}

static bool _run_on_ui(app_manager_ui_call_fn callback, void *arg)
{
    return app_manager_mailbox_call(callback, arg, UI_TIMEOUT_MS) == ESP_OK;
}

static bool _query_adapter(setup_wifi_adapter_t *adapter,
                           adapter_query_t *query)
{
    memset(query, 0, sizeof(*query));
    query->adapter = adapter;
    return _run_on_ui(_ui_query_adapter, query);
}

static bool _wait_status(wifi_service_state_t state,
                         wifi_service_status_snapshot_t *out)
{
    for (unsigned attempt = 0; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t snapshot;
        if (wifi_service_get_status(&snapshot) == ESP_OK &&
                snapshot.state == state)
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

static bool _wait_scan(wifi_service_scan_state_t state,
                       wifi_service_scan_snapshot_t *out)
{
    for (unsigned attempt = 0; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_scan_snapshot_t snapshot;
        if (wifi_service_get_scan_snapshot(&snapshot) == ESP_OK &&
                snapshot.state == state)
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

static unsigned _observer_status_state_count(observer_t *observer,
        wifi_service_state_t state)
{
    (void)pthread_mutex_lock(&observer->lock);
    unsigned count = observer->status_states[state];
    (void)pthread_mutex_unlock(&observer->lock);
    return count;
}

static unsigned _observer_scan_state_count(observer_t *observer,
        wifi_service_scan_state_t state)
{
    (void)pthread_mutex_lock(&observer->lock);
    unsigned count = observer->scan_states[state];
    (void)pthread_mutex_unlock(&observer->lock);
    return count;
}

static unsigned _observer_total_count(observer_t *observer)
{
    (void)pthread_mutex_lock(&observer->lock);
    unsigned count = observer->status_count + observer->scan_count;
    (void)pthread_mutex_unlock(&observer->lock);
    return count;
}

static bool _observer_copy_status(observer_t *observer,
                                  wifi_service_status_snapshot_t *snapshot,
                                  setup_wifi_status_scope_t *scope,
                                  setup_wifi_operation_kind_t *kind)
{
    (void)pthread_mutex_lock(&observer->lock);
    *snapshot = observer->status;
    *scope = observer->last_scope;
    *kind = observer->last_operation_kind;
    bool valid = observer->status_count > 0;
    (void)pthread_mutex_unlock(&observer->lock);
    return valid;
}

static bool _observer_copy_scan(observer_t *observer,
                                wifi_service_scan_snapshot_t *snapshot)
{
    (void)pthread_mutex_lock(&observer->lock);
    *snapshot = observer->scan;
    bool valid = observer->scan_count > 0;
    (void)pthread_mutex_unlock(&observer->lock);
    return valid;
}

static bool _observer_identity_ok(observer_t *observer)
{
    (void)pthread_mutex_lock(&observer->lock);
    bool valid = !observer->identity_violation;
    (void)pthread_mutex_unlock(&observer->lock);
    return valid;
}

static bool _wait_observer_status(observer_t *observer,
                                  wifi_service_state_t state,
                                  unsigned expected)
{
    for (unsigned attempt = 0; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (_observer_status_state_count(observer, state) >= expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_observer_scan(observer_t *observer,
                                wifi_service_scan_state_t state,
                                unsigned expected)
{
    for (unsigned attempt = 0; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (_observer_scan_state_count(observer, state) >= expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static esp_err_t _submit_event(wifi_service_port_event_type_t type,
                               uint32_t ipv4_address)
{
    const wifi_service_port_event_t event =
    {
        .type = type,
        .epoch = host_wifi_port_epoch(),
        .status = 0,
        .ipv4_address = ipv4_address,
        .disconnect_reason = 0,
        .scan_id = host_wifi_port_scan_id(),
    };
    return wifi_service_port_submit_event(&event);
}

static void _noop_callback(void *arg)
{
    (void)arg;
    atomic_fetch_add(&s_noop_count, 1U);
}

static bool _fill_mailbox(void)
{
    atomic_store(&s_noop_count, 0U);
    app_manager_mailbox_host_timer_pause(true);
    for (unsigned index = 0; index < EVENT_BUS_MAX_PENDING_UI_CALLBACKS;
            ++index)
    {
        CHECK(app_manager_mailbox_post(_noop_callback, NULL) == ESP_OK);
    }
    CHECK(app_manager_mailbox_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    return true;
}

static bool _drain_full_mailbox(void)
{
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (atomic_load(&s_noop_count) ==
                EVENT_BUS_MAX_PENDING_UI_CALLBACKS)
        {
            app_manager_mailbox_host_timer_pause(false);
            return _run_on_ui(_ui_barrier, NULL);
        }
        _sleep_one_ms();
    }
    app_manager_mailbox_host_timer_pause(false);
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
    record.channel = 6;
    record.security = security;
    host_wifi_port_set_scan_records(&record, 1, false);
}

static bool _run_scan(setup_wifi_adapter_t *adapter, observer_t *observer,
                      const char *ssid, unsigned expected_results)
{
    _set_scan_record(ssid, WIFI_SERVICE_SECURITY_PERSONAL);
    const unsigned running_before = _observer_scan_state_count(
                                        observer, WIFI_SERVICE_SCAN_RUNNING);
    CHECK(_run_on_ui(_ui_scan, adapter));
    CHECK(_wait_scan(WIFI_SERVICE_SCAN_RUNNING, NULL));
    CHECK(_wait_observer_scan(observer, WIFI_SERVICE_SCAN_RUNNING,
                              running_before + 1U));
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE, 0) == ESP_OK);
    CHECK(_wait_scan(WIFI_SERVICE_SCAN_RESULTS, NULL));
    CHECK(_wait_observer_scan(observer, WIFI_SERVICE_SCAN_RESULTS,
                              expected_results));
    CHECK(_wait_status(WIFI_SERVICE_STATE_IDLE, NULL));
    CHECK(_run_on_ui(_ui_barrier, NULL));
    return true;
}

static bool _run_pipeline(void)
{
    observer_t observer_a;
    observer_t observer_b;
    observer_t observer_c;
    setup_wifi_adapter_t adapter_a = {0};
    setup_wifi_adapter_t adapter_b = {0};
    setup_wifi_adapter_t adapter_c = {0};
    _observer_init(&observer_a);
    _observer_init(&observer_b);
    _observer_init(&observer_c);

    host_freertos_reset_controls();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    app_manager_ui_dispatch_fn dispatcher = NULL;
    CHECK(app_manager_get_ui_dispatch_fn(&dispatcher) == ESP_OK);
    CHECK(dispatcher != NULL);
    CHECK(event_bus_register_ui_dispatch(dispatcher) == ESP_OK);
    CHECK(_run_on_ui(_ui_capture_worker, NULL));
    CHECK(wifi_service_init() == ESP_OK);
    CHECK(_wait_status(WIFI_SERVICE_STATE_IDLE, NULL));

    open_args_t open_a =
    {
        .adapter = &adapter_a,
        .observer = &observer_a,
    };
    CHECK(_run_on_ui(_ui_open_adapter, &open_a));
    CHECK(_wait_observer_status(&observer_a, WIFI_SERVICE_STATE_IDLE, 1U));
    CHECK(_observer_identity_ok(&observer_a));
    CHECK(_run_scan(&adapter_a, &observer_a, "Initial AP", 1U));
    wifi_service_scan_snapshot_t delivered_scan;
    CHECK(_observer_copy_scan(&observer_a, &delivered_scan));
    CHECK(strcmp(delivered_scan.records[0].ssid, "Initial AP") == 0);
    CHECK(_observer_identity_ok(&observer_a));

    const unsigned stale_running = _observer_scan_state_count(
                                       &observer_a,
                                       WIFI_SERVICE_SCAN_RUNNING);
    CHECK(_run_on_ui(_ui_scan, &adapter_a));
    CHECK(_wait_observer_scan(&observer_a, WIFI_SERVICE_SCAN_RUNNING,
                              stale_running + 1U));
    adapter_query_t old_query;
    CHECK(_query_adapter(&adapter_a, &old_query));
    CHECK(old_query.session_id != 0 && old_query.operation_id != 0);
    host_wifi_port_gate(HOST_WIFI_PORT_SCAN_ABORT, true);
    CHECK(_run_on_ui(_ui_close_adapter, &adapter_a));
    CHECK(host_wifi_port_wait_gate(HOST_WIFI_PORT_SCAN_ABORT,
                                   TEST_TIMEOUT_MS));

    open_args_t open_b =
    {
        .adapter = &adapter_b,
        .observer = &observer_b,
    };
    CHECK(_run_on_ui(_ui_open_adapter, &open_b));
    adapter_query_t new_query;
    CHECK(_query_adapter(&adapter_b, &new_query));
    CHECK(new_query.session_id != 0 &&
          new_query.session_id != old_query.session_id);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE, 0) == ESP_OK);
    host_wifi_port_release_gate(HOST_WIFI_PORT_SCAN_ABORT);
    wifi_service_scan_snapshot_t canceled_scan;
    CHECK(_wait_scan(WIFI_SERVICE_SCAN_CANCELED, &canceled_scan));
    CHECK(canceled_scan.session_id == old_query.session_id);
    CHECK(canceled_scan.operation_id == old_query.operation_id);
    CHECK(_wait_status(WIFI_SERVICE_STATE_IDLE, NULL));
    CHECK(_run_on_ui(_ui_barrier, NULL));
    CHECK(_observer_scan_state_count(&observer_b,
                                     WIFI_SERVICE_SCAN_CANCELED) == 0U);
    CHECK(_observer_scan_state_count(&observer_b,
                                     WIFI_SERVICE_SCAN_RESULTS) == 0U);
    CHECK(_run_scan(&adapter_b, &observer_b, "Current AP", 1U));

    char ssid[WIFI_SERVICE_SSID_MAX_BYTES + 1U];
    uint8_t password[WIFI_SERVICE_PASSWORD_MAX_BYTES];
    memset(ssid, 0, sizeof(ssid));
    memcpy(ssid, "Current AP", sizeof("Current AP") - 1U);
    memset(password, 0, sizeof(password));
    memcpy(password, "password1", sizeof("password1") - 1U);
    connect_args_t connect =
    {
        .adapter = &adapter_b,
        .ssid = ssid,
        .ssid_length = sizeof("Current AP") - 1U,
        .security = WIFI_SERVICE_SECURITY_PERSONAL,
        .password = password,
        .password_length = sizeof("password1") - 1U,
    };
    CHECK(_run_on_ui(_ui_connect, &connect));
    for (size_t index = 0; index < sizeof(password); ++index)
    {
        CHECK(password[index] == 0);
    }
    memset(ssid, 'X', sizeof(ssid));
    CHECK(_query_adapter(&adapter_b, &new_query));
    CHECK(new_query.operation_kind == SETUP_WIFI_OPERATION_CONNECT);
    CHECK(_wait_status(WIFI_SERVICE_STATE_CONNECTING, NULL));
    CHECK(_wait_observer_status(&observer_b, WIFI_SERVICE_STATE_CONNECTING, 1U));
    wifi_service_port_credentials_t driver_credentials;
    CHECK(host_wifi_port_last_credentials(&driver_credentials));
    CHECK(driver_credentials.ssid_length == sizeof("Current AP") - 1U);
    CHECK(memcmp(driver_credentials.ssid, "Current AP",
                 driver_credentials.ssid_length) == 0);
    CHECK(driver_credentials.password_length == sizeof("password1") - 1U);
    CHECK(memcmp(driver_credentials.password, "password1",
                 driver_credentials.password_length) == 0);

    const unsigned waiting_before = _observer_status_state_count(
                                        &observer_b,
                                        WIFI_SERVICE_STATE_WAITING_IP);
    const unsigned ready_before = _observer_status_state_count(
                                      &observer_b,
                                      WIFI_SERVICE_STATE_IP_READY);
    CHECK(_fill_mailbox());
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_STA_CONNECTED, 0) == ESP_OK);
    wifi_service_status_snapshot_t cached_waiting;
    CHECK(_wait_status(WIFI_SERVICE_STATE_WAITING_IP, &cached_waiting));
    CHECK(cached_waiting.session_id == new_query.session_id);
    CHECK(cached_waiting.operation_id == new_query.operation_id);
    const uint32_t ipv4 = UINT32_C(0x0102a8c0);
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_GOT_IP, ipv4) == ESP_OK);
    wifi_service_status_snapshot_t cached_ready;
    CHECK(_wait_status(WIFI_SERVICE_STATE_IP_READY, &cached_ready));
    CHECK(cached_ready.generation != cached_waiting.generation);
    for (unsigned attempt = 0; attempt < 20U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(_observer_status_state_count(&observer_b,
                                       WIFI_SERVICE_STATE_WAITING_IP) ==
          waiting_before);
    CHECK(_observer_status_state_count(&observer_b,
                                       WIFI_SERVICE_STATE_IP_READY) ==
          ready_before);
    CHECK(_drain_full_mailbox());
    CHECK(_wait_observer_status(&observer_b, WIFI_SERVICE_STATE_IP_READY,
                                ready_before + 1U));
    CHECK(_observer_status_state_count(&observer_b,
                                       WIFI_SERVICE_STATE_WAITING_IP) ==
          waiting_before);
    wifi_service_status_snapshot_t delivered_ready;
    setup_wifi_status_scope_t delivered_scope;
    setup_wifi_operation_kind_t delivered_kind;
    CHECK(_observer_copy_status(&observer_b, &delivered_ready,
                                &delivered_scope, &delivered_kind));
    CHECK(delivered_ready.generation == cached_ready.generation);
    CHECK(delivered_ready.session_id == new_query.session_id);
    CHECK(delivered_ready.operation_id == new_query.operation_id);
    CHECK(delivered_ready.ipv4_address == ipv4);
    CHECK(delivered_scope == SETUP_WIFI_STATUS_OPERATION);
    CHECK(delivered_kind == SETUP_WIFI_OPERATION_CONNECT);
    CHECK(_observer_status_state_count(&observer_b,
                                       WIFI_SERVICE_STATE_IP_READY) ==
          ready_before + 1U);
    CHECK(_observer_identity_ok(&observer_b));

    const unsigned terminal_before = _observer_scan_state_count(
                                         &observer_b,
                                         WIFI_SERVICE_SCAN_RESULTS);
    _set_scan_record("Dropped On Stop", WIFI_SERVICE_SECURITY_OPEN);
    CHECK(_run_on_ui(_ui_scan, &adapter_b));
    CHECK(_wait_scan(WIFI_SERVICE_SCAN_RUNNING, NULL));
    CHECK(_run_on_ui(_ui_barrier, NULL));
    adapter_query_t stop_query;
    CHECK(_query_adapter(&adapter_b, &stop_query));
    CHECK(stop_query.session_id != 0 && stop_query.operation_id != 0);
    CHECK(stop_query.operation_kind == SETUP_WIFI_OPERATION_SCAN);
    CHECK(_fill_mailbox());
    CHECK(_submit_event(WIFI_SERVICE_PORT_EVENT_SCAN_DONE, 0) == ESP_OK);
    wifi_service_scan_snapshot_t unadmitted_scan;
    CHECK(_wait_scan(WIFI_SERVICE_SCAN_RESULTS, &unadmitted_scan));
    CHECK(unadmitted_scan.session_id == stop_query.session_id);
    CHECK(unadmitted_scan.operation_id == stop_query.operation_id);
    for (unsigned attempt = 0; attempt < 20U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(_observer_scan_state_count(&observer_b,
                                     WIFI_SERVICE_SCAN_RESULTS) ==
          terminal_before);
    const unsigned callbacks_before_deinit = _observer_total_count(&observer_b);
    CHECK(wifi_service_deinit(WIFI_SERVICE_WAIT_FOREVER) == ESP_OK);
    CHECK(!wifi_service_is_available());
    CHECK(_drain_full_mailbox());
    for (unsigned attempt = 0; attempt < 100U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(_run_on_ui(_ui_barrier, NULL));
    CHECK(_observer_total_count(&observer_b) == callbacks_before_deinit);
    CHECK(_run_on_ui(_ui_close_adapter, &adapter_b));

    const unsigned worker_creates = host_freertos_total_task_creates();
    CHECK(worker_creates == 1U);
    CHECK(wifi_service_init() == ESP_OK);
    CHECK(host_freertos_total_task_creates() == worker_creates);
    CHECK(_wait_status(WIFI_SERVICE_STATE_IDLE, NULL));
    open_args_t open_c =
    {
        .adapter = &adapter_c,
        .observer = &observer_c,
    };
    CHECK(_run_on_ui(_ui_open_adapter, &open_c));
    adapter_query_t reinit_query;
    CHECK(_query_adapter(&adapter_c, &reinit_query));
    CHECK(reinit_query.session_id != 0 &&
          reinit_query.session_id != stop_query.session_id);
    CHECK(_wait_observer_status(&observer_c, WIFI_SERVICE_STATE_IDLE, 1U));
    CHECK(_run_scan(&adapter_c, &observer_c, "After Reinit", 1U));
    CHECK(_observer_copy_scan(&observer_c, &delivered_scan));
    CHECK(strcmp(delivered_scan.records[0].ssid, "After Reinit") == 0);
    CHECK(_observer_identity_ok(&observer_c));
    CHECK(_run_on_ui(_ui_close_adapter, &adapter_c));
    CHECK(wifi_service_deinit(WIFI_SERVICE_WAIT_FOREVER) == ESP_OK);
    CHECK(host_wifi_port_is_clean_snapshot());
    CHECK(!host_wifi_port_thread_violation());
    CHECK(!host_wifi_port_scan_ownership_violation());

    CHECK(event_bus_unregister_ui_dispatch(dispatcher) == ESP_OK);
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    CHECK(host_freertos_live_queues() == 1U);
    /* WiFi retains three controls and the process event bus retains two. */
    CHECK(host_freertos_live_semaphores() == 5U);
    CHECK(host_freertos_live_tasks() == 1U);
    _observer_deinit(&observer_c);
    _observer_deinit(&observer_b);
    _observer_deinit(&observer_a);
    return true;
}

int main(void)
{
    if (!_run_pipeline())
    {
        return 1;
    }
    puts("production connectivity pipeline tests passed");
    return 0;
}

#include "apps_integration_runtime.h"

#include "app_manager.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_theme.h"
#include "event_bus.h"
#include "host_wifi_service.h"
#include "power_service.h"
#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define UI_TIMEOUT_MS 1000U
#define WAIT_ATTEMPTS 2000U
#define BUILTIN_APP_COUNT 4U

EVENT_BUS_DEFINE_ID(CROSS_LAYER_TEST_MSG);

extern const app_builtin_app_desc_t _app_builtin_apps_start[];
extern const app_builtin_app_desc_t _app_builtin_apps_end[];

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

static esp_err_t _ui_barrier(void *arg)
{
    (void)arg;
    return ESP_OK;
}

static void _noop_callback(void *arg)
{
    (void)arg;
    atomic_fetch_add(&s_noop_count, 1U);
}

static void _subscription_probe_callback(event_bus_msg_id_t msg_id,
        uint32_t sub_type, const void *payload, size_t payload_size,
        void *user_data)
{
    (void)msg_id;
    (void)sub_type;
    (void)payload;
    (void)payload_size;
    (void)user_data;
}

static esp_err_t _click_action_on_ui(void *arg)
{
    return host_lv_click_action(arg) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _click_back_on_ui(void *arg)
{
    (void)arg;
    return host_lv_click_back() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

typedef struct text_query
{
    const char *text;
    bool found;
} text_query_t;

static esp_err_t _query_text_on_ui(void *arg)
{
    text_query_t *query = arg;
    query->found = host_lv_has_text(query->text);
    return ESP_OK;
}

static esp_err_t _lifecycle_deinit_on_ui(void *arg)
{
    (void)arg;
    app_manager_lifecycle_deinit();
    return ESP_OK;
}

static bool _ui_has_text(const char *text)
{
    text_query_t query = {.text = text};
    assert(app_manager_ui_call(_query_text_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    return query.found;
}

static void _click_action(const char *title)
{
    assert(app_manager_ui_call(_click_action_on_ui, (void *)title,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static void _click_back(void)
{
    assert(app_manager_ui_call(_click_back_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static bool _wait_for_text(const char *text)
{
    bool found = false;
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS && !found; ++attempt)
    {
        found = _ui_has_text(text);
        if (!found)
        {
            _sleep_one_ms();
        }
    }
    return found;
}

static bool _wait_for_active(const char *app_id)
{
    bool active = false;
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS && !active; ++attempt)
    {
        active = app_manager_is_actived(app_id);
        if (!active)
        {
            _sleep_one_ms();
        }
    }
    return active;
}

static void _assert_event_slot_headroom(size_t occupied)
{
    event_bus_sub_handle_t handles[EVENT_BUS_MAX_SUBSCRIBERS];
    assert(occupied <= EVENT_BUS_MAX_SUBSCRIBERS);
    const size_t available = EVENT_BUS_MAX_SUBSCRIBERS - occupied;
    for (size_t index = 0; index < available; ++index)
    {
        assert(event_bus_subscribe(CROSS_LAYER_TEST_MSG,
                                   EVENT_BUS_SUB_TYPE_ANY,
                                   _subscription_probe_callback, NULL,
                                   EVENT_BUS_DISPATCH_UI,
                                   &handles[index]) == ESP_OK);
    }
    event_bus_sub_handle_t overflow = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(CROSS_LAYER_TEST_MSG,
                               EVENT_BUS_SUB_TYPE_ANY,
                               _subscription_probe_callback, NULL,
                               EVENT_BUS_DISPATCH_UI,
                               &overflow) == ESP_ERR_NO_MEM);
    for (size_t index = 0; index < available; ++index)
    {
        assert(event_bus_unsubscribe(handles[index]) == ESP_OK);
    }
}

static void _initialize_stack(void)
{
    host_runtime_reset();
    assert(app_theme_init() == ESP_OK);
    for (int id = 0; id < APP_THEME_FONT_MAX; ++id)
    {
        assert(app_theme_set_font((app_theme_font_id_t)id,
                                  LV_FONT_DEFAULT) == ESP_OK);
    }
    assert(app_manager_mailbox_init() == ESP_OK);
    assert(event_bus_init() == ESP_OK);

    app_manager_ui_dispatch_fn dispatcher = NULL;
    assert(app_manager_get_ui_dispatch_fn(&dispatcher) == ESP_OK);
    assert(dispatcher != NULL);
    assert(event_bus_register_ui_dispatch(dispatcher) == ESP_OK);

    ptrdiff_t descriptor_count =
        _app_builtin_apps_end - _app_builtin_apps_start;
    assert(descriptor_count == (ptrdiff_t)BUILTIN_APP_COUNT);
    assert(app_manager_register_builtin_apps(
               _app_builtin_apps_start,
               (size_t)descriptor_count) == ESP_OK);
    assert(app_manager_builtin_discover() == (int)BUILTIN_APP_COUNT);
    assert(app_manager_lifecycle_init(0) == ESP_OK);
}

static void _test_real_app_navigation(void)
{
    assert(app_manager_run(APP_MANAGER_ID_HOME) == ESP_OK);
    assert(_ui_has_text("08:30"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));

    power_service_snapshot_t snapshot =
    {
        .info = {
            .battery_voltage_mv = 4020,
            .battery_percent = 90,
            .is_charging = true,
            .is_vbus_connected = true,
        },
        .sampled_at_ms = 234567,
        .valid = true,
    };
    assert(event_bus_publish(
               POWER_SERVICE_MSG,
               POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE,
               &snapshot, sizeof(snapshot),
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("90% | Charging"));

    _click_action("Applications");
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_MENU, "root"));
    /* Home must cancel its power subscription as part of real ONPAUSE. */
    _assert_event_slot_headroom(0);

    _click_action("Settings");
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "root"));

    _click_action("Power");
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "power"));
    assert(_ui_has_text("3910 mV"));
    _click_back();
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "power"));

    assert(app_manager_run(APP_MANAGER_ID_SETUP) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_MENU, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETUP, "root"));
    assert(host_wifi_service_current_session() != 0);
    assert(host_wifi_service_current_operation() != 0);

    wifi_service_scan_snapshot_t scan =
    {
        .session_id = host_wifi_service_current_session(),
        .operation_id = host_wifi_service_current_operation(),
        .state = WIFI_SERVICE_SCAN_RESULTS,
        .last_error = ESP_OK,
        .record_count = 1,
    };
    memcpy(scan.records[0].ssid, "Cross Layer AP",
           sizeof("Cross Layer AP"));
    scan.records[0].rssi = -45;
    scan.records[0].channel = 6;
    scan.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    assert(host_wifi_service_publish_scan(&scan) == ESP_OK);
    assert(_wait_for_text("Select a network"));
    _click_action("Cross Layer AP");
    wifi_service_status_snapshot_t ready =
    {
        .session_id = host_wifi_service_current_session(),
        .operation_id = host_wifi_service_current_operation(),
        .state = WIFI_SERVICE_STATE_IP_READY,
        .last_error = ESP_OK,
        .ipv4_address = UINT32_C(0x0100000a),
        .available = true,
        .desired_connected = true,
    };
    memcpy(ready.ssid, "Cross Layer AP", sizeof("Cross Layer AP"));
    assert(host_wifi_service_publish_status(&ready) == ESP_OK);
    assert(_wait_for_text("Connected"));

    const unsigned disconnects = host_wifi_service_call_count(
                                     HOST_WIFI_SERVICE_CALL_REQUEST_DISCONNECT);
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(host_wifi_service_call_count(
               HOST_WIFI_SERVICE_CALL_REQUEST_DISCONNECT) == disconnects);
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
}

static void _test_latest_power_backpressure(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    atomic_store(&s_noop_count, 0U);
    app_manager_mailbox_host_timer_pause(true);

    power_service_snapshot_t snapshot =
    {
        .info =
        {
            .battery_voltage_mv = 3900,
            .battery_percent = 0,
            .is_charging = false,
            .is_vbus_connected = false,
        },
        .sampled_at_ms = 0,
        .valid = true,
    };
    for (unsigned index = 0; index < 1000U; ++index)
    {
        snapshot.info.battery_percent = (int8_t)(index % 100U);
        snapshot.sampled_at_ms = (int64_t)index;
        assert(event_bus_publish(
                   POWER_SERVICE_MSG,
                   POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE,
                   &snapshot, sizeof(snapshot),
                   EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    }

    for (unsigned index = 0; index < 23U; ++index)
    {
        assert(app_manager_ui_post(_noop_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_noop_count) == 0U);

    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS &&
            atomic_load(&s_noop_count) != 23U; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load(&s_noop_count) == 23U);
    app_manager_mailbox_host_timer_pause(false);
    assert(_ui_has_text("99% | On battery"));
}

static esp_err_t _publish_status_and_exit_setup_on_ui(void *arg)
{
    const wifi_service_status_snapshot_t *snapshot = arg;
    esp_err_t result = event_bus_publish(
                           WIFI_SERVICE_MSG,
                           WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           snapshot, sizeof(*snapshot),
                           EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = app_manager_exit(APP_MANAGER_ID_SETUP);

exit:
    return result;
}

static void _test_latest_wifi_backpressure_and_reopen(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_manager_run(APP_MANAGER_ID_SETUP) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(_wait_for_text("Connected"));
    _click_action("Scan networks");

    const wifi_service_session_id_t old_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t old_operation =
        host_wifi_service_current_operation();
    assert(old_session != 0 && old_operation != 0);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    atomic_store(&s_noop_count, 0U);
    app_manager_mailbox_host_timer_pause(true);
    wifi_service_status_snapshot_t status =
    {
        .state = WIFI_SERVICE_STATE_IDLE,
        .last_error = ESP_OK,
        .available = true,
    };
    wifi_service_scan_snapshot_t scan =
    {
        .session_id = old_session,
        .operation_id = old_operation,
        .state = WIFI_SERVICE_SCAN_RESULTS,
        .last_error = ESP_OK,
        .record_count = 1,
    };
    scan.records[0].rssi = -50;
    scan.records[0].channel = 6;
    scan.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    for (unsigned index = 0; index < 1000U; ++index)
    {
        status.last_error = (int32_t)index;
        (void)snprintf(scan.records[0].ssid,
                       sizeof(scan.records[0].ssid),
                       "WiFi %03u", index);
        assert(host_wifi_service_publish_status(&status) == ESP_OK);
        assert(host_wifi_service_publish_scan(&scan) == ESP_OK);
    }

    for (unsigned index = 0; index < 22U; ++index)
    {
        assert(app_manager_ui_post(_noop_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_noop_count) == 0U);
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS &&
            atomic_load(&s_noop_count) != 22U; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load(&s_noop_count) == 22U);
    app_manager_mailbox_host_timer_pause(false);
    assert(_ui_has_text("WiFi 999"));

    _click_action("WiFi 999");
    const wifi_service_operation_id_t connect_operation =
        host_wifi_service_current_operation();
    assert(connect_operation != 0);
    wifi_service_status_snapshot_t queued =
    {
        .generation = UINT64_C(100000),
        .session_id = old_session,
        .operation_id = connect_operation,
        .state = WIFI_SERVICE_STATE_CONNECTING,
        .last_error = ESP_OK,
        .available = true,
        .desired_connected = true,
    };
    memcpy(queued.ssid, "WiFi 999", sizeof("WiFi 999"));
    assert(app_manager_ui_call(_publish_status_and_exit_setup_on_ui, &queued,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    _assert_event_slot_headroom(1);

    assert(app_manager_run(APP_MANAGER_ID_SETUP) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    const wifi_service_session_id_t new_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t new_operation =
        host_wifi_service_current_operation();
    assert(new_session != 0 && new_session != old_session);
    assert(new_operation != 0);

    wifi_service_scan_snapshot_t stale =
    {
        .generation = UINT64_C(200000),
        .session_id = old_session,
        .operation_id = old_operation,
        .state = WIFI_SERVICE_SCAN_RESULTS,
        .last_error = ESP_OK,
        .record_count = 1,
    };
    memcpy(stale.records[0].ssid, "Old Session AP",
           sizeof("Old Session AP"));
    stale.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    assert(host_wifi_service_publish_raw_scan(&stale, sizeof(stale)) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(!_ui_has_text("Old Session AP"));
    assert(_ui_has_text("Scanning..."));

    scan.session_id = new_session;
    scan.operation_id = new_operation;
    memcpy(scan.records[0].ssid, "Current Session AP",
           sizeof("Current Session AP"));
    assert(host_wifi_service_publish_scan(&scan) == ESP_OK);
    assert(_wait_for_text("Current Session AP"));
    assert(app_manager_exit(APP_MANAGER_ID_SETUP) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    _assert_event_slot_headroom(1);
}

int main(void)
{
    _initialize_stack();
    _test_real_app_navigation();
    _test_latest_power_backpressure();
    _test_latest_wifi_backpressure_and_reopen();

    assert(app_manager_ui_call(_lifecycle_deinit_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);

    host_task_shutdown();
    assert(app_manager_mailbox_deinit() == ESP_OK);
    app_theme_deinit();
    puts("production cross-layer integration tests passed");
    return 0;
}

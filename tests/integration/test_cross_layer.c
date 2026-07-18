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
#define LIFECYCLE_OBSERVATION_CAPACITY 256U
#define LIFECYCLE_ID_BYTES 32U

EVENT_BUS_DEFINE_ID(CROSS_LAYER_TEST_MSG);

extern const app_builtin_app_desc_t _app_builtin_apps_start[];
extern const app_builtin_app_desc_t _app_builtin_apps_end[];

static atomic_uint s_noop_count;
static atomic_uint s_queued_scan_probe_count;

typedef struct lifecycle_observation
{
    char app_id[LIFECYCLE_ID_BYTES];
    char page_id[LIFECYCLE_ID_BYTES];
    app_manager_msg_type_t message;
    app_manager_lifecycle_observer_phase_t phase;
    size_t live_objects;
    size_t live_timers;
} lifecycle_observation_t;

typedef struct lv_resource_counts
{
    size_t objects;
    size_t timers;
} lv_resource_counts_t;

typedef struct queued_scan_pause_request
{
    wifi_service_scan_snapshot_t snapshot;
    event_bus_sub_handle_t probe_subscription;
} queued_scan_pause_request_t;

static lifecycle_observation_t
s_lifecycle_observations[LIFECYCLE_OBSERVATION_CAPACITY];
static size_t s_lifecycle_observation_count;

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

static void _lifecycle_observer(
    const char *app_id, const char *page_id,
    app_manager_msg_type_t message,
    app_manager_lifecycle_observer_phase_t phase)
{
    assert(s_lifecycle_observation_count <
           LIFECYCLE_OBSERVATION_CAPACITY);
    lifecycle_observation_t *observation =
        &s_lifecycle_observations[s_lifecycle_observation_count++];
    (void)snprintf(observation->app_id, sizeof(observation->app_id),
                   "%s", app_id);
    (void)snprintf(observation->page_id, sizeof(observation->page_id),
                   "%s", page_id);
    observation->message = message;
    observation->phase = phase;
    observation->live_objects = host_lv_live_object_count();
    observation->live_timers = host_lv_live_timer_count();
}

static size_t _lifecycle_observed(const char *app_id, const char *page_id,
                                  app_manager_msg_type_t message,
                                  app_manager_lifecycle_observer_phase_t phase)
{
    size_t count = 0;
    for (size_t index = 0; index < s_lifecycle_observation_count; ++index)
    {
        const lifecycle_observation_t *observation =
            &s_lifecycle_observations[index];
        if (strcmp(observation->app_id, app_id) == 0 &&
                strcmp(observation->page_id, page_id) == 0 &&
                observation->message == message &&
                observation->phase == phase)
        {
            ++count;
        }
    }
    return count;
}

static esp_err_t _query_lv_resources_on_ui(void *arg)
{
    lv_resource_counts_t *counts = arg;
    counts->objects = host_lv_live_object_count();
    counts->timers = host_lv_live_timer_count();
    return ESP_OK;
}

static lv_resource_counts_t _lv_resource_counts(void)
{
    lv_resource_counts_t counts = {0};
    assert(app_manager_ui_call(_query_lv_resources_on_ui, &counts,
                               UI_TIMEOUT_MS) == ESP_OK);
    return counts;
}

static esp_err_t _screen_pause_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_screen_pause();
}

static esp_err_t _screen_resume_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_screen_resume();
}

static void _blocked_page_handler(app_manager_msg_type_t message, void *param)
{
    (void)message;
    (void)param;
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

static void _queued_scan_probe_callback(event_bus_msg_id_t msg_id,
                                        uint32_t sub_type, const void *payload,
                                        size_t payload_size, void *user_data)
{
    (void)user_data;
    assert(msg_id == WIFI_SERVICE_MSG);
    assert(sub_type == WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT);
    assert(payload != NULL);
    assert(payload_size == sizeof(wifi_service_scan_snapshot_t));
    atomic_fetch_add(&s_queued_scan_probe_count, 1U);
}

static esp_err_t _publish_scan_and_pause_on_ui(void *arg)
{
    queued_scan_pause_request_t *request = arg;
    esp_err_t result = host_wifi_service_publish_raw_scan(
                           &request->snapshot, sizeof(request->snapshot));
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = event_bus_unsubscribe(request->probe_subscription);
    if (result != ESP_OK)
    {
        goto exit;
    }
    request->probe_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    result = app_manager_lifecycle_screen_pause();

exit:
    return result;
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
    return app_manager_lifecycle_deinit();
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
    memset(s_lifecycle_observations, 0,
           sizeof(s_lifecycle_observations));
    s_lifecycle_observation_count = 0;
    app_manager_lifecycle_host_set_observer(_lifecycle_observer);
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

    _click_action("About");
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "about"));
    assert(_ui_has_text("Compact ESP32-S3 wearable platform"));
    _click_back();
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "about"));

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

static void _assert_real_page_start_contract(void)
{
    static const struct
    {
        const char *app_id;
        const char *page_id;
    } expected_pages[] =
    {
        {APP_MANAGER_ID_HOME, "root"},
        {APP_MANAGER_ID_MENU, "root"},
        {APP_MANAGER_ID_SETTINGS, "root"},
        {APP_MANAGER_ID_SETTINGS, "power"},
        {APP_MANAGER_ID_SETTINGS, "about"},
        {APP_MANAGER_ID_SETUP, "root"},
    };

    for (size_t page = 0;
            page < sizeof(expected_pages) / sizeof(expected_pages[0]); ++page)
    {
        size_t starts_before = 0;
        size_t starts = 0;
        for (size_t index = 0; index < s_lifecycle_observation_count; ++index)
        {
            const lifecycle_observation_t *observation =
                &s_lifecycle_observations[index];
            if (strcmp(observation->app_id,
                       expected_pages[page].app_id) == 0 &&
                    strcmp(observation->page_id,
                           expected_pages[page].page_id) == 0 &&
                    observation->message == APP_MANAGER_MSG_ONSTART)
            {
                assert(observation->live_objects == 0);
                assert(observation->live_timers == 0);
                if (observation->phase ==
                        APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE)
                {
                    ++starts_before;
                }
                else
                {
                    ++starts;
                }
            }
        }
        assert(starts > 0);
        assert(starts == starts_before);
    }
}

static void _exercise_real_page_screen_lifecycle(
    const char *app_id, const char *page_id, const char *visible_text,
    size_t active_subscriptions)
{
    assert(app_manager_is_actived(app_id));
    assert(app_page_is_actived(app_id, page_id));
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(active_subscriptions);

    const size_t starts_before = _lifecycle_observed(
                                     app_id, page_id,
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      app_id, page_id,
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t pauses_before = _lifecycle_observed(
                                     app_id, page_id,
                                     APP_MANAGER_MSG_ONPAUSE,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    resources = _lv_resource_counts();
    assert(resources.objects == 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(app_id));
    assert(!app_page_is_actived(app_id, page_id));
    assert(app_manager_get_active_app_id() == NULL);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(app_id));
    assert(app_page_is_actived(app_id, page_id));
    assert(app_manager_is_page_present(app_id, page_id));
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(active_subscriptions);
    assert(_ui_has_text(visible_text));

    assert(_lifecycle_observed(
               app_id, page_id, APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               app_id, page_id, APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);
    assert(_lifecycle_observed(
               app_id, page_id, APP_MANAGER_MSG_ONPAUSE,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == pauses_before + 1U);
}

static void _test_other_real_app_screen_lifecycles(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_manager_get_running_apps() == 1U);

    assert(app_manager_run(APP_MANAGER_ID_MENU) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_MENU, "root", "Applications", 0);

    assert(app_manager_run(APP_MANAGER_ID_SETTINGS) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "root", "Settings", 0);

    _click_action("Power");
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "power"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "power", "3910 mV", 1);
    _click_back();
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "root"));

    _click_action("About");
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "about"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "about",
        "Compact ESP32-S3 wearable platform", 0);
    _click_back();
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "root"));

    assert(app_manager_exit(APP_MANAGER_ID_SETTINGS) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    assert(app_manager_exit(APP_MANAGER_ID_MENU) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(app_manager_get_running_apps() == 1U);
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(_ui_has_text("08:30"));
    _assert_event_slot_headroom(1);
}

static void _test_home_screen_lifecycle(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_running_apps() == 1U);
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 1U);
    _assert_event_slot_headroom(1);

    const size_t starts_before = _lifecycle_observed(
                                     APP_MANAGER_ID_HOME, "root",
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      APP_MANAGER_ID_HOME, "root",
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t pauses_before = _lifecycle_observed(
                                     APP_MANAGER_ID_HOME, "root",
                                     APP_MANAGER_MSG_ONPAUSE,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    resources = _lv_resource_counts();
    assert(resources.objects == 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(!app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_active_app_id() == NULL);
    assert(app_manager_get_running_apps() == 1U);
    assert(!app_manager_is_all_closed());

    assert(app_manager_run(APP_MANAGER_ID_MENU) == ESP_ERR_INVALID_STATE);
    assert(app_manager_exit(APP_MANAGER_ID_HOME) == ESP_ERR_INVALID_STATE);
    assert(app_manager_create_page_ext(
               "blocked", _blocked_page_handler, NULL, 0) ==
           ESP_ERR_INVALID_STATE);
    assert(app_manager_create_page_for_app_ext(
               APP_MANAGER_ID_HOME, "blocked", _blocked_page_handler,
               NULL, 0) == ESP_ERR_INVALID_STATE);
    assert(app_manager_goback() == ESP_ERR_INVALID_STATE);
    assert(app_manager_goback_to_page("root") == ESP_ERR_INVALID_STATE);
    assert(app_manager_remove_page("root") == ESP_ERR_INVALID_STATE);
    assert(app_manager_set_page_userdata("root", &resources) ==
           ESP_ERR_INVALID_STATE);
    app_manager_self_exit();
    assert(app_manager_get_running_apps() == 1U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_running_apps() == 1U);
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 1U);
    _assert_event_slot_headroom(1);
    assert(_ui_has_text("08:30"));

    assert(_lifecycle_observed(
               APP_MANAGER_ID_HOME, "root", APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_HOME, "root", APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_HOME, "root", APP_MANAGER_MSG_ONPAUSE,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == pauses_before + 1U);
}

static void _test_setup_screen_lifecycle(void)
{
    assert(app_manager_run(APP_MANAGER_ID_SETUP) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(_wait_for_text("Scanning..."));

    const wifi_service_session_id_t old_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t old_operation =
        host_wifi_service_current_operation();
    assert(old_session != 0 && old_operation != 0);
    const size_t starts_before = _lifecycle_observed(
                                     APP_MANAGER_ID_SETUP, "root",
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      APP_MANAGER_ID_SETUP, "root",
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    queued_scan_pause_request_t queued_pause =
    {
        .snapshot = {
            .generation = UINT64_C(300000),
            .session_id = old_session,
            .operation_id = old_operation,
            .state = WIFI_SERVICE_SCAN_RESULTS,
            .last_error = ESP_OK,
            .record_count = 1,
        },
        .probe_subscription = EVENT_BUS_SUB_HANDLE_INVALID,
    };
    memcpy(queued_pause.snapshot.records[0].ssid, "Paused Session AP",
           sizeof("Paused Session AP"));
    queued_pause.snapshot.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    atomic_store(&s_queued_scan_probe_count, 0U);
    assert(event_bus_subscribe(
               WIFI_SERVICE_MSG,
               WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
               _queued_scan_probe_callback, NULL, EVENT_BUS_DISPATCH_UI,
               &queued_pause.probe_subscription) == ESP_OK);
    _assert_event_slot_headroom(3);

    assert(app_manager_ui_call(_publish_scan_and_pause_on_ui, &queued_pause,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(queued_pause.probe_subscription ==
           EVENT_BUS_SUB_HANDLE_INVALID);
    assert(host_wifi_service_current_session() == 0);
    assert(host_wifi_service_current_operation() == 0);
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(atomic_load(&s_queued_scan_probe_count) == 0U);
    assert(resources.objects == 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(APP_MANAGER_ID_SETUP));
    assert(!app_page_is_actived(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_get_running_apps() == 2U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    const wifi_service_session_id_t new_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t new_operation =
        host_wifi_service_current_operation();
    assert(new_session != 0 && new_session != old_session);
    assert(new_operation != 0 && new_operation != old_operation);
    assert(app_manager_is_actived(APP_MANAGER_ID_SETUP));
    assert(app_page_is_actived(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_get_running_apps() == 2U);
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(2);

    assert(_lifecycle_observed(
               APP_MANAGER_ID_SETUP, "root", APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_SETUP, "root", APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);
    assert(!_ui_has_text("Paused Session AP"));
    assert(_ui_has_text("Scanning..."));

    wifi_service_scan_snapshot_t current = queued_pause.snapshot;
    current.generation++;
    current.session_id = new_session;
    current.operation_id = new_operation;
    memcpy(current.records[0].ssid, "Resumed Session AP",
           sizeof("Resumed Session AP"));
    assert(host_wifi_service_publish_scan(&current) == ESP_OK);
    assert(_wait_for_text("Resumed Session AP"));

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
    _assert_real_page_start_contract();
    _test_other_real_app_screen_lifecycles();
    _test_home_screen_lifecycle();
    _test_setup_screen_lifecycle();

    assert(app_manager_lifecycle_shutdown_begin_and_wait() == ESP_OK);
    assert(app_manager_ui_call(_lifecycle_deinit_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);

    host_task_shutdown();
    assert(app_manager_mailbox_deinit() == ESP_OK);
    app_theme_deinit();
    puts("production cross-layer integration tests passed");
    return 0;
}

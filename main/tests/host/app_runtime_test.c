#include "app_runtime.h"

#include "app_manager.h"
#include "app_manager_config.h"
#include "app_runtime_pm.h"
#include "ble_service.h"
#include "bsp_hal.h"
#include "event_bus.h"
#include "fs_storage/fs_storage.h"
#include "network_runtime.h"
#include "nv_storage.h"
#include "power_service.h"
#include "system_pm.h"
#include "time_service.h"
#include "wifi_service.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_EVENT_CAPACITY 256U

typedef enum
{
    TEST_EVENT_NONE = 0,
    TEST_EVENT_LOG_INIT,
    TEST_EVENT_NV_INIT,
    TEST_EVENT_FS_INIT,
    TEST_EVENT_EVENT_BUS_INIT,
    TEST_EVENT_BSP_INIT,
    TEST_EVENT_TIME_REGISTER_RTC,
    TEST_EVENT_TIME_INIT,
    TEST_EVENT_PM_BUILD_CONFIG,
    TEST_EVENT_SYSTEM_PM_INIT,
    TEST_EVENT_APP_MANAGER_INIT,
    TEST_EVENT_GET_UI_DISPATCH,
    TEST_EVENT_REGISTER_UI_DISPATCH,
    TEST_EVENT_REGISTER_WAKE_REQUESTER,
    TEST_EVENT_PM_PREPARE_POWER,
    TEST_EVENT_POWER_INIT,
    TEST_EVENT_NETWORK_INIT,
    TEST_EVENT_WIFI_INIT,
    TEST_EVENT_BLE_INIT,
    TEST_EVENT_BUILTIN_DISCOVER,
    TEST_EVENT_APP_NAVIGATE,
    TEST_EVENT_DISPLAY_COMMIT,
    TEST_EVENT_SCREEN_COMMIT,
    TEST_EVENT_STARTUP_COMMIT,
    TEST_EVENT_SYSTEM_PM_CANCEL,
    TEST_EVENT_BLE_DEINIT,
    TEST_EVENT_WIFI_DEINIT,
    TEST_EVENT_POWER_DEINIT,
    TEST_EVENT_UNREGISTER_WAKE_REQUESTER,
    TEST_EVENT_APP_MANAGER_DEINIT,
    TEST_EVENT_UNREGISTER_UI_DISPATCH,
    TEST_EVENT_SYSTEM_PM_DEINIT,
    TEST_EVENT_TIME_DEINIT,
    TEST_EVENT_BSP_DEINIT,
    TEST_EVENT_FS_DEINIT,
    TEST_EVENT_NV_DEINIT,
} test_event_t;

typedef struct test_runtime
{
    test_event_t events[TEST_EVENT_CAPACITY];
    size_t event_count;
    test_event_t failure_event;
    esp_err_t failure_result;
    bsp_capabilities_t capabilities;
    bool network_ready;
    bool wifi_cleanup_pending;
    bool required_apps_present;
} test_runtime_t;

static test_runtime_t s_test;

static const app_manager_app_desc_t s_required_apps[] =
{
    {
        .name = "home", .id = APP_MANAGER_ID_HOME, .root_page_id = "root",
        .type = APP_MANAGER_APP_BUILTIN,
    },
    {
        .name = "menu", .id = APP_MANAGER_ID_MENU, .root_page_id = "root",
        .type = APP_MANAGER_APP_BUILTIN,
    },
    {
        .name = "settings", .id = APP_MANAGER_ID_SETTINGS,
        .root_page_id = "root", .type = APP_MANAGER_APP_BUILTIN,
    },
    {
        .name = "setup", .id = APP_MANAGER_ID_SETUP, .root_page_id = "root",
        .type = APP_MANAGER_APP_BUILTIN,
    },
};

static void _test_record(test_event_t event)
{
    assert(s_test.event_count < TEST_EVENT_CAPACITY);
    s_test.events[s_test.event_count++] = event;
}

static esp_err_t _test_result(test_event_t event)
{
    _test_record(event);
    return s_test.failure_event == event ? s_test.failure_result : ESP_OK;
}

static void _test_reset(void)
{
    memset(&s_test, 0, sizeof(s_test));
    s_test.capabilities = BSP_CAPABILITY_DISPLAY | BSP_CAPABILITY_TOUCH |
                          BSP_CAPABILITY_INPUT | BSP_CAPABILITY_RTC |
                          BSP_CAPABILITY_POWER;
    s_test.network_ready = true;
    s_test.required_apps_present = true;
    s_test.failure_result = ESP_FAIL;
}

static void _test_clear_events(void)
{
    s_test.event_count = 0;
}

static bool _test_event_seen(test_event_t event)
{
    bool seen = false;
    for (size_t index = 0; index < s_test.event_count; ++index)
    {
        if (s_test.events[index] == event)
        {
            seen = true;
            break;
        }
    }
    return seen;
}

static void _test_expect_events(const test_event_t *expected, size_t count)
{
    assert(s_test.event_count == count);
    for (size_t index = 0; index < count; ++index)
    {
        assert(s_test.events[index] == expected[index]);
    }
}

static bool _test_rtc_available(void)
{
    return true;
}

static esp_err_t _test_rtc_read(struct tm *timeinfo)
{
    (void)timeinfo;
    return ESP_OK;
}

static esp_err_t _test_rtc_write(const struct tm *timeinfo)
{
    (void)timeinfo;
    return ESP_OK;
}

static bool _test_screen_available(void)
{
    return true;
}

static esp_err_t _test_screen_operation(void)
{
    return ESP_OK;
}

static esp_err_t _test_screen_commit(void)
{
    return _test_result(TEST_EVENT_SCREEN_COMMIT);
}

static bool _test_screen_state(void)
{
    return false;
}

static esp_err_t _test_set_brightness(uint8_t brightness)
{
    (void)brightness;
    return ESP_OK;
}

static uint8_t _test_get_brightness(void)
{
    return 50U;
}

static esp_err_t _test_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

static const bsp_rtc_ops_t s_rtc_ops =
{
    .is_available = _test_rtc_available,
    .read = _test_rtc_read,
    .write = _test_rtc_write,
};

static const bsp_screen_ops_t s_screen_ops =
{
    .is_available = _test_screen_available,
    .suspend = _test_screen_operation,
    .resume_prepare = _test_screen_operation,
    .resume_commit = _test_screen_commit,
    .is_suspended = _test_screen_state,
    .is_suspend_committed = _test_screen_state,
    .set_brightness = _test_set_brightness,
    .set_brightness_temp = _test_set_brightness,
    .get_brightness = _test_get_brightness,
    .set_enabled = _test_set_enabled,
    .set_power = _test_set_enabled,
};

static const bsp_display_port_t s_display_port;

static esp_err_t _test_ui_dispatch(void (*callback)(void *), void *argument)
{
    (void)callback;
    (void)argument;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "host error";
}

void test_log_write(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
    va_list arguments;
    va_start(arguments, format);
    va_end(arguments);
}

esp_err_t mt_log_init(void)
{
    return _test_result(TEST_EVENT_LOG_INIT);
}

esp_err_t nv_storage_init(void)
{
    return _test_result(TEST_EVENT_NV_INIT);
}

esp_err_t nv_storage_deinit(void)
{
    return _test_result(TEST_EVENT_NV_DEINIT);
}

esp_err_t fs_storage_init(void)
{
    return _test_result(TEST_EVENT_FS_INIT);
}

esp_err_t fs_storage_deinit(void)
{
    return _test_result(TEST_EVENT_FS_DEINIT);
}

bool fs_storage_is_initialized(void)
{
    return true;
}

esp_err_t event_bus_init(void)
{
    return _test_result(TEST_EVENT_EVENT_BUS_INIT);
}

esp_err_t event_bus_register_ui_dispatch(event_bus_ui_dispatch_fn dispatch)
{
    assert(dispatch == _test_ui_dispatch);
    return _test_result(TEST_EVENT_REGISTER_UI_DISPATCH);
}

esp_err_t event_bus_unregister_ui_dispatch(
    event_bus_ui_dispatch_fn expected_dispatch)
{
    assert(expected_dispatch == _test_ui_dispatch);
    return _test_result(TEST_EVENT_UNREGISTER_UI_DISPATCH);
}

esp_err_t event_bus_register_wake_requester(
    event_bus_wake_request_fn request_wake)
{
    assert(request_wake == app_manager_pm_notify_user_activity);
    return _test_result(TEST_EVENT_REGISTER_WAKE_REQUESTER);
}

esp_err_t event_bus_unregister_wake_requester(
    event_bus_wake_request_fn expected_request_wake)
{
    assert(expected_request_wake == app_manager_pm_notify_user_activity);
    return _test_result(TEST_EVENT_UNREGISTER_WAKE_REQUESTER);
}

esp_err_t bsp_init(void)
{
    return _test_result(TEST_EVENT_BSP_INIT);
}

esp_err_t bsp_deinit(void)
{
    return _test_result(TEST_EVENT_BSP_DEINIT);
}

bsp_capabilities_t bsp_get_capabilities(void)
{
    return s_test.capabilities;
}

const bsp_rtc_ops_t *bsp_hal_get_rtc(void)
{
    return &s_rtc_ops;
}

const bsp_screen_ops_t *bsp_hal_get_screen(void)
{
    return &s_screen_ops;
}

const bsp_display_port_t *bsp_display_get_port(void)
{
    return &s_display_port;
}

esp_err_t time_service_register_rtc_ops(const time_service_rtc_ops_t *ops)
{
    assert(ops != NULL);
    return _test_result(TEST_EVENT_TIME_REGISTER_RTC);
}

esp_err_t time_service_init(void)
{
    return _test_result(TEST_EVENT_TIME_INIT);
}

esp_err_t time_service_deinit(void)
{
    return _test_result(TEST_EVENT_TIME_DEINIT);
}

esp_err_t app_runtime_pm_build_system_config(system_pm_config_t *config)
{
    assert(config != NULL);
    memset(config, 0, sizeof(*config));
    return _test_result(TEST_EVENT_PM_BUILD_CONFIG);
}

app_manager_input_ops_t app_runtime_pm_get_input_ops(void)
{
    return (app_manager_input_ops_t)
    {
        0
    };
}

app_manager_standby_ops_t app_runtime_pm_get_standby_ops(void)
{
    return (app_manager_standby_ops_t)
    {
        0
    };
}

esp_err_t app_runtime_pm_prepare_power(bsp_capabilities_t capabilities)
{
    assert(capabilities == s_test.capabilities);
    return _test_result(TEST_EVENT_PM_PREPARE_POWER);
}

void app_runtime_pm_reset(void)
{
}

void app_runtime_pm_close_admission(void)
{
}

void app_runtime_pm_open_admission(void)
{
}

void app_runtime_pm_clear_power(void)
{
}

void app_runtime_pm_set_wifi_participant(bool enabled)
{
    (void)enabled;
}

void app_runtime_pm_detach_bsp(void)
{
}

esp_err_t system_pm_init(const system_pm_config_t *config)
{
    assert(config != NULL);
    return _test_result(TEST_EVENT_SYSTEM_PM_INIT);
}

esp_err_t system_pm_deinit(void)
{
    return _test_result(TEST_EVENT_SYSTEM_PM_DEINIT);
}

esp_err_t system_pm_cancel_standby(void)
{
    return _test_result(TEST_EVENT_SYSTEM_PM_CANCEL);
}

esp_err_t app_manager_init(const struct app_manager_config *config)
{
    assert(config != NULL);
    const app_manager_config_t *app_config = config;
    assert(app_config->max_resident_apps == APP_MANAGER_MAX_RESIDENT_APPS);
    assert(app_config->resident_policy == APP_MANAGER_RESIDENT_REJECT);
    assert(app_config->app_forward_transition.effect ==
           APP_MANAGER_TRANSITION_FADE);
    assert(app_config->app_back_transition.effect ==
           APP_MANAGER_TRANSITION_FADE);
    assert(app_config->page_forward_transition.effect ==
           APP_MANAGER_TRANSITION_PUSH_LEFT);
    assert(app_config->page_back_transition.effect ==
           APP_MANAGER_TRANSITION_PUSH_RIGHT);
    assert(app_config->app_forward_transition.duration_ms ==
           APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS);
    return _test_result(TEST_EVENT_APP_MANAGER_INIT);
}

esp_err_t app_manager_deinit(void)
{
    return _test_result(TEST_EVENT_APP_MANAGER_DEINIT);
}

esp_err_t app_manager_get_ui_dispatch_fn(app_manager_ui_dispatch_fn *output)
{
    esp_err_t result = _test_result(TEST_EVENT_GET_UI_DISPATCH);
    if (result == ESP_OK)
    {
        *output = _test_ui_dispatch;
    }
    return result;
}

esp_err_t app_manager_pm_notify_user_activity(void)
{
    return ESP_OK;
}

int app_manager_builtin_discover(void)
{
    _test_record(TEST_EVENT_BUILTIN_DISCOVER);
    return (int)(sizeof(s_required_apps) / sizeof(s_required_apps[0]));
}

const app_manager_app_desc_t *app_manager_builtin_list_open(void)
{
    return s_test.required_apps_present ? &s_required_apps[0] : NULL;
}

const app_manager_app_desc_t *app_manager_builtin_list_get_next(
    const app_manager_app_desc_t *previous)
{
    const app_manager_app_desc_t *next = previous + 1;
    const app_manager_app_desc_t *limit =
        &s_required_apps[sizeof(s_required_apps) / sizeof(s_required_apps[0])];
    return next < limit ? next : NULL;
}

esp_err_t app_manager_navigate(const app_manager_nav_request_t *request,
                               uint32_t timeout_ms)
{
    assert(request != NULL);
    assert(request->operation == APP_MANAGER_NAV_OP_RUN);
    assert(strcmp(request->app_id, APP_MANAGER_ID_HOME) == 0);
    assert(request->transition.effect == APP_MANAGER_TRANSITION_NONE);
    assert(timeout_ms == UINT32_MAX);
    return _test_result(TEST_EVENT_APP_NAVIGATE);
}

esp_err_t app_manager_display_commit_initial(void)
{
    esp_err_t result = _test_result(TEST_EVENT_DISPLAY_COMMIT);
    if (result == ESP_OK)
    {
        result = _test_result(TEST_EVENT_SCREEN_COMMIT);
    }
    return result;
}

esp_err_t app_manager_startup_commit(void)
{
    return _test_result(TEST_EVENT_STARTUP_COMMIT);
}

esp_err_t power_service_init(void)
{
    return _test_result(TEST_EVENT_POWER_INIT);
}

esp_err_t power_service_deinit(void)
{
    return _test_result(TEST_EVENT_POWER_DEINIT);
}

esp_err_t network_runtime_init(void)
{
    return _test_result(TEST_EVENT_NETWORK_INIT);
}

bool network_runtime_is_ready(void)
{
    return s_test.network_ready;
}

network_runtime_status_t network_runtime_get_status(void)
{
    return (network_runtime_status_t)
    {
        .ready = s_test.network_ready,
    };
}

esp_err_t wifi_service_init(void)
{
    return _test_result(TEST_EVENT_WIFI_INIT);
}

esp_err_t wifi_service_deinit(uint32_t timeout_ms)
{
    assert(timeout_ms == WIFI_SERVICE_WAIT_FOREVER);
    return _test_result(TEST_EVENT_WIFI_DEINIT);
}

bool wifi_service_is_cleanup_pending(void)
{
    return s_test.wifi_cleanup_pending;
}

esp_err_t ble_service_init(void)
{
    return _test_result(TEST_EVENT_BLE_INIT);
}

esp_err_t ble_service_deinit(void)
{
    return _test_result(TEST_EVENT_BLE_DEINIT);
}

static void _test_successful_lifecycle(void)
{
    static const test_event_t expected[] =
    {
        TEST_EVENT_LOG_INIT,
        TEST_EVENT_NV_INIT,
        TEST_EVENT_FS_INIT,
        TEST_EVENT_EVENT_BUS_INIT,
        TEST_EVENT_BSP_INIT,
        TEST_EVENT_TIME_REGISTER_RTC,
        TEST_EVENT_TIME_INIT,
        TEST_EVENT_PM_BUILD_CONFIG,
        TEST_EVENT_SYSTEM_PM_INIT,
        TEST_EVENT_APP_MANAGER_INIT,
        TEST_EVENT_GET_UI_DISPATCH,
        TEST_EVENT_REGISTER_UI_DISPATCH,
        TEST_EVENT_REGISTER_WAKE_REQUESTER,
        TEST_EVENT_PM_PREPARE_POWER,
        TEST_EVENT_POWER_INIT,
        TEST_EVENT_BUILTIN_DISCOVER,
        TEST_EVENT_APP_NAVIGATE,
        TEST_EVENT_DISPLAY_COMMIT,
        TEST_EVENT_SCREEN_COMMIT,
        TEST_EVENT_NETWORK_INIT,
        TEST_EVENT_WIFI_INIT,
        TEST_EVENT_BLE_INIT,
        TEST_EVENT_STARTUP_COMMIT,
        TEST_EVENT_SYSTEM_PM_CANCEL,
        TEST_EVENT_BLE_DEINIT,
        TEST_EVENT_WIFI_DEINIT,
        TEST_EVENT_POWER_DEINIT,
        TEST_EVENT_UNREGISTER_WAKE_REQUESTER,
        TEST_EVENT_APP_MANAGER_DEINIT,
        TEST_EVENT_UNREGISTER_UI_DISPATCH,
        TEST_EVENT_SYSTEM_PM_DEINIT,
        TEST_EVENT_TIME_DEINIT,
        TEST_EVENT_BSP_DEINIT,
        TEST_EVENT_FS_DEINIT,
        TEST_EVENT_NV_DEINIT,
    };

    _test_reset();
    assert(app_runtime_start() == ESP_OK);
    assert(app_runtime_is_running());
    size_t event_count = s_test.event_count;
    assert(app_runtime_start() == ESP_OK);
    assert(s_test.event_count == event_count);
    assert(app_runtime_stop() == ESP_OK);
    assert(!app_runtime_is_running());
    _test_expect_events(expected, sizeof(expected) / sizeof(expected[0]));
    event_count = s_test.event_count;
    assert(app_runtime_stop() == ESP_OK);
    assert(s_test.event_count == event_count);
}

static void _test_fatal_start_failures(void)
{
    static const test_event_t fatal_events[] =
    {
        TEST_EVENT_LOG_INIT,
        TEST_EVENT_NV_INIT,
        TEST_EVENT_FS_INIT,
        TEST_EVENT_EVENT_BUS_INIT,
        TEST_EVENT_BSP_INIT,
        TEST_EVENT_TIME_REGISTER_RTC,
        TEST_EVENT_TIME_INIT,
        TEST_EVENT_PM_BUILD_CONFIG,
        TEST_EVENT_SYSTEM_PM_INIT,
        TEST_EVENT_APP_MANAGER_INIT,
        TEST_EVENT_GET_UI_DISPATCH,
        TEST_EVENT_REGISTER_UI_DISPATCH,
        TEST_EVENT_REGISTER_WAKE_REQUESTER,
        TEST_EVENT_PM_PREPARE_POWER,
        TEST_EVENT_POWER_INIT,
        TEST_EVENT_BLE_INIT,
        TEST_EVENT_APP_NAVIGATE,
        TEST_EVENT_DISPLAY_COMMIT,
        TEST_EVENT_SCREEN_COMMIT,
        TEST_EVENT_STARTUP_COMMIT,
    };

    for (size_t index = 0;
            index < sizeof(fatal_events) / sizeof(fatal_events[0]); ++index)
    {
        _test_reset();
        s_test.failure_event = fatal_events[index];
        assert(app_runtime_start() == ESP_FAIL);
        assert(!app_runtime_is_running());
        size_t event_count = s_test.event_count;
        assert(app_runtime_stop() == ESP_OK);
        assert(s_test.event_count == event_count);
    }
}

static void _test_cleanup_retry_before_restart(void)
{
    static const test_event_t first_cleanup[] =
    {
        TEST_EVENT_SYSTEM_PM_CANCEL,
        TEST_EVENT_BLE_DEINIT,
        TEST_EVENT_WIFI_DEINIT,
    };

    _test_reset();
    assert(app_runtime_start() == ESP_OK);
    _test_clear_events();
    s_test.failure_event = TEST_EVENT_WIFI_DEINIT;
    assert(app_runtime_stop() == ESP_FAIL);
    assert(!app_runtime_is_running());
    _test_expect_events(first_cleanup,
                        sizeof(first_cleanup) / sizeof(first_cleanup[0]));

    _test_clear_events();
    s_test.failure_event = TEST_EVENT_NONE;
    assert(app_runtime_start() == ESP_OK);
    assert(app_runtime_is_running());
    assert(s_test.events[0] == TEST_EVENT_WIFI_DEINIT);
    assert(!_test_event_seen(TEST_EVENT_BLE_DEINIT));
    assert(_test_event_seen(TEST_EVENT_LOG_INIT));
    assert(app_runtime_stop() == ESP_OK);
}

static void _test_every_cleanup_failure_is_retryable(void)
{
    static const test_event_t cleanup_events[] =
    {
        TEST_EVENT_SYSTEM_PM_CANCEL,
        TEST_EVENT_BLE_DEINIT,
        TEST_EVENT_WIFI_DEINIT,
        TEST_EVENT_POWER_DEINIT,
        TEST_EVENT_UNREGISTER_WAKE_REQUESTER,
        TEST_EVENT_APP_MANAGER_DEINIT,
        TEST_EVENT_UNREGISTER_UI_DISPATCH,
        TEST_EVENT_SYSTEM_PM_DEINIT,
        TEST_EVENT_TIME_DEINIT,
        TEST_EVENT_BSP_DEINIT,
        TEST_EVENT_FS_DEINIT,
        TEST_EVENT_NV_DEINIT,
    };

    for (size_t index = 0;
            index < sizeof(cleanup_events) / sizeof(cleanup_events[0]); ++index)
    {
        _test_reset();
        assert(app_runtime_start() == ESP_OK);
        _test_clear_events();
        s_test.failure_event = cleanup_events[index];
        assert(app_runtime_stop() == ESP_FAIL);
        assert(!app_runtime_is_running());
        assert(_test_event_seen(cleanup_events[index]));

        _test_clear_events();
        s_test.failure_event = TEST_EVENT_NONE;
        assert(app_runtime_stop() == ESP_OK);
        assert(!app_runtime_is_running());
        if (cleanup_events[index] != TEST_EVENT_SYSTEM_PM_CANCEL)
        {
            assert(!_test_event_seen(TEST_EVENT_SYSTEM_PM_CANCEL));
        }
    }
}

static void _test_degradable_connectivity_failures(void)
{
    _test_reset();
    s_test.failure_event = TEST_EVENT_NETWORK_INIT;
    s_test.network_ready = false;
    assert(app_runtime_start() == ESP_OK);
    assert(!_test_event_seen(TEST_EVENT_WIFI_INIT));
    assert(_test_event_seen(TEST_EVENT_BLE_INIT));
    _test_clear_events();
    assert(app_runtime_stop() == ESP_OK);
    assert(!_test_event_seen(TEST_EVENT_WIFI_DEINIT));

    _test_reset();
    s_test.failure_event = TEST_EVENT_WIFI_INIT;
    assert(app_runtime_start() == ESP_OK);
    assert(_test_event_seen(TEST_EVENT_BLE_INIT));
    _test_clear_events();
    assert(app_runtime_stop() == ESP_OK);
    assert(!_test_event_seen(TEST_EVENT_WIFI_DEINIT));
}

static void _test_wifi_cleanup_pending_is_fatal(void)
{
    _test_reset();
    s_test.failure_event = TEST_EVENT_WIFI_INIT;
    s_test.wifi_cleanup_pending = true;
    assert(app_runtime_start() == ESP_FAIL);
    assert(!app_runtime_is_running());
    assert(_test_event_seen(TEST_EVENT_WIFI_DEINIT));
    assert(!_test_event_seen(TEST_EVENT_BLE_INIT));
}

int main(void)
{
    _test_successful_lifecycle();
    _test_fatal_start_failures();
    _test_cleanup_retry_before_restart();
    _test_every_cleanup_failure_is_retryable();
    _test_degradable_connectivity_failures();
    _test_wifi_cleanup_pending_is_fatal();
    return 0;
}

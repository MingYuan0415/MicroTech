#include "app_runtime.h"

#include "app_manager.h"
#include "app_manager_config.h"
#include "app_runtime_pm.h"
#include "chore_service.h"
#include "connectivity_manager.h"
#include "display_benchmark.h"
#include "audio_service.h"
#include "ble_nimble_port.h"
#include "device_link_service.h"
#include "factory_reset_service.h"
#include "bsp_hal.h"
#include "event_bus.h"
#include "fs_storage/fs_storage.h"
#include "imu_service.h"
#include "network_runtime.h"
#include "nv_storage.h"
#include "power_service.h"
#include "sd_storage_service.h"
#include "system_pm.h"
#include "time_service.h"
#include "weather_service.h"

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
    TEST_EVENT_FACTORY_RESET_INIT,
    TEST_EVENT_FACTORY_RESET_RECOVERY_PENDING,
    TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE,
    TEST_EVENT_FS_INIT,
    TEST_EVENT_EVENT_BUS_INIT,
    TEST_EVENT_BSP_INIT,
    TEST_EVENT_TIME_REGISTER_RTC,
    TEST_EVENT_TIME_INIT,
    TEST_EVENT_CHORE_INIT,
    TEST_EVENT_PM_BUILD_CONFIG,
    TEST_EVENT_SYSTEM_PM_INIT,
    TEST_EVENT_APP_MANAGER_INIT,
    TEST_EVENT_GET_UI_DISPATCH,
    TEST_EVENT_REGISTER_UI_DISPATCH,
    TEST_EVENT_REGISTER_WAKE_REQUESTER,
    TEST_EVENT_PM_PREPARE_POWER,
    TEST_EVENT_POWER_INIT,
    TEST_EVENT_IMU_REGISTER,
    TEST_EVENT_IMU_INIT,
    TEST_EVENT_AUDIO_INIT,
    TEST_EVENT_SD_REGISTER,
    TEST_EVENT_SD_INIT,
    TEST_EVENT_NETWORK_INIT,
    TEST_EVENT_CONNECTIVITY_INIT,
    TEST_EVENT_CONNECTIVITY_SUBSCRIBE,
    TEST_EVENT_CONNECTIVITY_GET_STATUS,
    TEST_EVENT_TIME_NETWORK_READY,
    TEST_EVENT_BLE_INIT,
    TEST_EVENT_FACTORY_RESET_COMPLETE,
    TEST_EVENT_DEVICE_LINK_RELEASE_GATE,
    TEST_EVENT_BUILTIN_DISCOVER,
    TEST_EVENT_APP_NAVIGATE,
    TEST_EVENT_DISPLAY_COMMIT,
    TEST_EVENT_SCREEN_COMMIT,
    TEST_EVENT_STARTUP_COMMIT,
    TEST_EVENT_DISPLAY_BENCHMARK_START,
    TEST_EVENT_DISPLAY_BENCHMARK_STOP,
    TEST_EVENT_SYSTEM_PM_CANCEL,
    TEST_EVENT_BLE_DEINIT,
    TEST_EVENT_CONNECTIVITY_DEINIT,
    TEST_EVENT_CONNECTIVITY_UNSUBSCRIBE,
    TEST_EVENT_SD_DEINIT,
    TEST_EVENT_AUDIO_DEINIT,
    TEST_EVENT_IMU_DEINIT,
    TEST_EVENT_POWER_DEINIT,
    TEST_EVENT_UNREGISTER_WAKE_REQUESTER,
    TEST_EVENT_APP_MANAGER_DEINIT,
    TEST_EVENT_UNREGISTER_UI_DISPATCH,
    TEST_EVENT_SYSTEM_PM_DEINIT,
    TEST_EVENT_CHORE_DEINIT,
    TEST_EVENT_TIME_DEINIT,
    TEST_EVENT_BSP_DEINIT,
    TEST_EVENT_FS_DEINIT,
    TEST_EVENT_FACTORY_RESET_DEINIT,
    TEST_EVENT_NV_DEINIT,
} test_event_t;

typedef struct test_runtime
{
    test_event_t events[TEST_EVENT_CAPACITY];
    size_t event_count;
    test_event_t failure_event;
    esp_err_t failure_result;
    test_event_t secondary_failure_event;
    esp_err_t secondary_failure_result;
    bsp_capabilities_t capabilities;
    bool network_ready;
    bool time_network_ready;
    bool required_apps_present;
    bool connectivity_participant;
    bool device_link_participant;
    bool imu_participant;
    bool audio_participant;
    bool time_participant;
    bool weather_participant;
    bool weather_network_ready;
    bool factory_reset_marker_durable;
    bool device_link_started_gated;
    uint32_t weather_ipv4_address;
    bool sd_mounted;
    uint32_t imu_sample_rate_hz;
    esp_err_t sd_mount_result;
    sd_storage_service_mount_ops_t sd_mount_ops;
    imu_service_imu_ops_t imu_ops;
    connectivity_manager_state_t connectivity_state;
    event_bus_cb_t connectivity_callback;
    void *connectivity_callback_context;
} test_runtime_t;

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);

static test_runtime_t s_test;

static const app_manager_app_desc_t s_required_apps[] =
{
    {
        .name = "home", .id = APP_MANAGER_ID_HOME, .root_page_id = "root",
    },
    {
        .name = "menu", .id = APP_MANAGER_ID_MENU, .root_page_id = "root",
    },
    {
        .name = "settings", .id = APP_MANAGER_ID_SETTINGS,
        .root_page_id = "root",
    },
    {
        .name = "setup", .id = APP_MANAGER_ID_SETUP, .root_page_id = "root",
    },
    {
        .name = "weather", .id = APP_MANAGER_ID_WEATHER,
        .root_page_id = "root",
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
    if (s_test.failure_event == event)
    {
        return s_test.failure_result;
    }
    return s_test.secondary_failure_event == event ?
           s_test.secondary_failure_result : ESP_OK;
}

static void _test_reset(void)
{
    memset(&s_test, 0, sizeof(s_test));
    s_test.capabilities = BSP_CAPABILITY_DISPLAY | BSP_CAPABILITY_TOUCH |
                          BSP_CAPABILITY_INPUT | BSP_CAPABILITY_RTC |
                          BSP_CAPABILITY_POWER | BSP_CAPABILITY_IMU |
                          BSP_CAPABILITY_AUDIO | BSP_CAPABILITY_SD;
    s_test.network_ready = true;
    s_test.required_apps_present = true;
    s_test.connectivity_state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    s_test.failure_result = ESP_FAIL;
    s_test.secondary_failure_result = ESP_FAIL;
    s_test.sd_mounted = true;
    s_test.sd_mount_result = ESP_OK;
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

static size_t _test_event_index(test_event_t event)
{
    for (size_t index = 0; index < s_test.event_count; ++index)
    {
        if (s_test.events[index] == event)
        {
            return index;
        }
    }
    return SIZE_MAX;
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

static esp_err_t _test_rtc_alarm_configure(
    const bsp_rtc_alarm_config_t *config)
{
    assert(config != NULL);
    return ESP_OK;
}

static esp_err_t _test_rtc_alarm_disable(void)
{
    return ESP_OK;
}

static esp_err_t _test_rtc_alarm_get_status(bsp_rtc_alarm_status_t *status)
{
    assert(status != NULL);
    memset(status, 0, sizeof(*status));
    return ESP_OK;
}

static esp_err_t _test_rtc_alarm_clear(void)
{
    return ESP_OK;
}

static esp_err_t _test_rtc_alarm_poll_interrupt(bool *active)
{
    assert(active != NULL);
    *active = false;
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
    .alarm_configure = _test_rtc_alarm_configure,
    .alarm_disable = _test_rtc_alarm_disable,
    .alarm_get_status = _test_rtc_alarm_get_status,
    .alarm_clear = _test_rtc_alarm_clear,
    .alarm_poll_interrupt = _test_rtc_alarm_poll_interrupt,
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

static bool _test_imu_available(void)
{
    return true;
}

static esp_err_t _test_imu_read(bsp_imu_sample_t *sample)
{
    assert(sample != NULL);
    memset(sample, 0, sizeof(*sample));
    return ESP_OK;
}

static esp_err_t _test_imu_configure(uint32_t sample_rate_hz)
{
    s_test.imu_sample_rate_hz = sample_rate_hz;
    return ESP_OK;
}

static esp_err_t _test_imu_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

static bool _test_imu_data_ready(void)
{
    return true;
}

static esp_err_t _test_imu_interrupt_level(bool *active)
{
    assert(active != NULL);
    *active = false;
    return ESP_OK;
}

static const bsp_imu_ops_t s_imu_ops =
{
    .is_available = _test_imu_available,
    .configure = _test_imu_configure,
    .read = _test_imu_read,
    .set_enabled = _test_imu_set_enabled,
    .get_data_ready = _test_imu_data_ready,
    .get_interrupt_level = _test_imu_interrupt_level,
};

static bool _test_sd_available(void)
{
    return true;
}

static esp_err_t _test_sd_mount(const bsp_sd_config_t *config)
{
    assert(config != NULL);
    return s_test.sd_mount_result;
}

static esp_err_t _test_sd_unmount(void)
{
    s_test.sd_mounted = false;
    return ESP_OK;
}

static bool _test_sd_mounted(void)
{
    return s_test.sd_mounted;
}

static const char *_test_sd_mount_point(void)
{
    return "/sdcard";
}

static const bsp_sd_ops_t s_sd_ops =
{
    .is_available = _test_sd_available,
    .mount = _test_sd_mount,
    .unmount = _test_sd_unmount,
    .is_mounted = _test_sd_mounted,
    .get_mount_point = _test_sd_mount_point,
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

esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t callback, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    assert(msg_id == CONNECTIVITY_MANAGER_MSG);
    assert(sub_type ==
           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT);
    assert(callback != NULL);
    assert(context == EVENT_BUS_DISPATCH_PUBLISHER);
    assert(out_handle != NULL);
    const esp_err_t result = _test_result(TEST_EVENT_CONNECTIVITY_SUBSCRIBE);
    if (result == ESP_OK)
    {
        s_test.connectivity_callback = callback;
        s_test.connectivity_callback_context = user_data;
        *out_handle = 1U;
    }
    return result;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    assert(handle == 1U);
    const esp_err_t result = _test_result(
                                 TEST_EVENT_CONNECTIVITY_UNSUBSCRIBE);
    if (result == ESP_OK)
    {
        s_test.connectivity_callback = NULL;
        s_test.connectivity_callback_context = NULL;
    }
    return result;
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

const bsp_imu_ops_t *bsp_hal_get_imu(void)
{
    return &s_imu_ops;
}

const bsp_sd_ops_t *bsp_hal_get_sd(void)
{
    return &s_sd_ops;
}

const bsp_display_port_t *bsp_display_get_port(void)
{
    return &s_display_port;
}

esp_err_t time_service_register_rtc_ops(const time_service_rtc_ops_t *ops)
{
    assert(ops != NULL);
    assert(ops->alarm_configure != NULL);
    assert(ops->alarm_disable != NULL);
    assert(ops->alarm_get_status != NULL);
    assert(ops->alarm_clear != NULL);
    assert(ops->alarm_poll_interrupt != NULL);
    return _test_result(TEST_EVENT_TIME_REGISTER_RTC);
}

esp_err_t time_service_init(const time_service_config_t *config)
{
    assert(config != NULL);
    assert(strcmp(config->timezone, "CST-8") == 0);
    assert(strcmp(config->sntp_server, "pool.ntp.org") == 0);
    assert(config->task_priority == 4U);
    return _test_result(TEST_EVENT_TIME_INIT);
}

esp_err_t time_service_deinit(void)
{
    return _test_result(TEST_EVENT_TIME_DEINIT);
}

esp_err_t time_service_set_network_ready(bool ready)
{
    s_test.time_network_ready = ready;
    return _test_result(TEST_EVENT_TIME_NETWORK_READY);
}

esp_err_t chore_service_init(const chore_service_config_t *config)
{
    assert(config != NULL);
    assert(config->task_priority == 4U);
    assert(config->warning_duration_ms == 500U);
    return _test_result(TEST_EVENT_CHORE_INIT);
}

esp_err_t chore_service_deinit(uint32_t timeout_ms)
{
    assert(timeout_ms == CHORE_SERVICE_WAIT_FOREVER);
    return _test_result(TEST_EVENT_CHORE_DEINIT);
}

esp_err_t weather_service_init(const weather_service_config_t *config)
{
    assert(config != NULL);
    assert(strcmp(config->server_base_url,
                  "https://weather.example.com") == 0);
    assert(strcmp(config->device_token, "example-device-token") == 0);
    assert(strcmp(config->cache_directory, "/data") == 0);
    assert(config->task_priority == 4U);
    assert(config->current_refresh_seconds == 1200U);
    assert(config->alerts_refresh_seconds == 600U);
    assert(config->hourly_refresh_seconds == 3600U);
    assert(config->daily_refresh_seconds == 14400U);
    assert(config->manual_refresh_min_seconds == 60U);
    assert(!config->allow_private_http);
    return ESP_OK;
}

esp_err_t weather_service_deinit(uint32_t timeout_ms)
{
    assert(timeout_ms == WEATHER_SERVICE_WAIT_FOREVER);
    return ESP_OK;
}

esp_err_t weather_service_set_network_ready(bool ready,
        uint32_t ipv4_address)
{
    s_test.weather_network_ready = ready;
    s_test.weather_ipv4_address = ipv4_address;
    return ESP_OK;
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

void app_runtime_pm_set_connectivity_participant(bool enabled)
{
    s_test.connectivity_participant = enabled;
}

void app_runtime_pm_set_imu_participant(bool enabled)
{
    s_test.imu_participant = enabled;
}

void app_runtime_pm_set_audio_participant(bool enabled)
{
    s_test.audio_participant = enabled;
}

void app_runtime_pm_set_time_participant(bool enabled)
{
    s_test.time_participant = enabled;
}

void app_runtime_pm_set_weather_participant(bool enabled)
{
    s_test.weather_participant = enabled;
}

void app_runtime_pm_detach_bsp(void)
{
}

esp_err_t system_pm_init(const system_pm_config_t *config)
{
    assert(config != NULL);
    assert(config->task_priority == 5U);
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
    assert(app_config->max_resident_apps == 4U);
    assert(app_config->resident_policy ==
           APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND);
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
    assert(app_config->control_task_priority == 5U);
    assert(app_config->resource_file_count == 1U);
    assert(app_config->resource_checksum == 0x4303U);
    assert(app_config->image_resources == NULL);
    assert(app_config->image_resource_count == 0U);
    assert(app_config->image_resource_count == 0U);
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

esp_err_t display_benchmark_start(const display_benchmark_config_t *config)
{
    assert(config != NULL);
    return _test_result(TEST_EVENT_DISPLAY_BENCHMARK_START);
}

esp_err_t display_benchmark_stop(void)
{
    return _test_result(TEST_EVENT_DISPLAY_BENCHMARK_STOP);
}

esp_err_t power_service_init(const power_service_config_t *config)
{
    assert(config != NULL);
    assert(config->poll_interval_ms == 5000U);
    assert(config->irq_poll_interval_ms == 100U);
    assert(config->task_priority == 4U);
    return _test_result(TEST_EVENT_POWER_INIT);
}

esp_err_t power_service_deinit(void)
{
    return _test_result(TEST_EVENT_POWER_DEINIT);
}

esp_err_t imu_service_register_imu_ops(const imu_service_imu_ops_t *ops)
{
    assert(ops != NULL);
    assert(ops->is_available != NULL);
    assert(ops->configure != NULL);
    assert(ops->read != NULL);
    assert(ops->set_enabled != NULL);
    assert(ops->poll_interrupt != NULL);
    const esp_err_t result = _test_result(TEST_EVENT_IMU_REGISTER);
    if (result == ESP_OK)
    {
        s_test.imu_ops = *ops;
    }
    return result;
}

esp_err_t imu_service_init(const imu_service_config_t *config)
{
    assert(config != NULL);
    assert(config->sample_rate_hz == 100U);
    assert(config->task_priority == 6U);
    esp_err_t result = _test_result(TEST_EVENT_IMU_INIT);
    if (result == ESP_OK && s_test.imu_ops.configure != NULL)
    {
        result = s_test.imu_ops.configure(config->sample_rate_hz);
    }
    return result;
}

esp_err_t imu_service_deinit(void)
{
    esp_err_t result = _test_result(TEST_EVENT_IMU_DEINIT);
    if (result == ESP_OK && s_test.imu_ops.set_enabled != NULL)
    {
        result = s_test.imu_ops.set_enabled(false);
    }
    return result;
}

esp_err_t audio_service_init(const audio_service_init_config_t *config)
{
    assert(config != NULL);
    assert(config->stream.sample_rate_hz == 16000U);
    assert(config->stream.bits_per_sample == 16U);
    assert(config->stream.channels == 2U);
    assert(config->stream.mclk_multiple == 384U);
    assert(config->volume_percent == 60U);
    assert(!config->muted);
    assert(config->pa_enabled);
    return _test_result(TEST_EVENT_AUDIO_INIT);
}

esp_err_t audio_service_deinit(void)
{
    return _test_result(TEST_EVENT_AUDIO_DEINIT);
}

esp_err_t sd_storage_service_register_mount_ops(
    const sd_storage_service_mount_ops_t *ops)
{
    assert(ops != NULL);
    assert(ops->context == &s_sd_ops);
    assert(ops->mount != NULL);
    assert(ops->unmount != NULL);
    assert(ops->is_mounted != NULL);
    esp_err_t result = _test_result(TEST_EVENT_SD_REGISTER);
    if (result == ESP_OK)
    {
        s_test.sd_mount_ops = *ops;
    }
    return result;
}

esp_err_t sd_storage_service_init(const sd_storage_service_config_t *config)
{
    assert(config != NULL);
    assert(strcmp(config->mount_path, "/sdcard") == 0);
    assert(config->max_files == 5);
    assert(config->allocation_unit_size == 16U * 1024U);
    return _test_result(TEST_EVENT_SD_INIT);
}

esp_err_t sd_storage_service_deinit(void)
{
    return _test_result(TEST_EVENT_SD_DEINIT);
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

esp_err_t connectivity_manager_init(
    const connectivity_manager_config_t *config)
{
    assert(config != NULL);
    assert(config->task_priority == 4U);
    assert(config->wifi_task_priority == 4U);
    return _test_result(TEST_EVENT_CONNECTIVITY_INIT);
}

esp_err_t connectivity_manager_clear_persisted_profile(void)
{
    return _test_result(TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE);
}

esp_err_t connectivity_manager_deinit(uint32_t timeout_ms)
{
    assert(timeout_ms == CONNECTIVITY_MANAGER_WAIT_FOREVER);
    return _test_result(TEST_EVENT_CONNECTIVITY_DEINIT);
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    const esp_err_t result = _test_result(
                                 TEST_EVENT_CONNECTIVITY_GET_STATUS);
    if (result == ESP_OK)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = s_test.connectivity_state;
        snapshot->ipv4_address = s_test.connectivity_state ==
                                 CONNECTIVITY_MANAGER_STATE_IP_READY ?
                                 UINT32_C(0x0102a8c0) : 0U;
        snapshot->available = true;
        snapshot->radio_available = true;
    }
    return result;
}

static const ble_runtime_host_port_t s_test_runtime_port =
{
    .init = NULL,
    .start = NULL,
    .stop = NULL,
    .deinit = NULL,
};

const ble_runtime_host_port_t *ble_nimble_port_get(void)
{
    return &s_test_runtime_port;
}

esp_err_t device_link_service_init(
    const device_link_service_config_t *config)
{
    assert(config != NULL);
    assert(config->runtime_port == &s_test_runtime_port);
    assert(config->task_priority == 4U);
    assert(config->window_ms == 120000U);
    assert(config->firmware_major == 0U);
    assert(config->firmware_minor == 1U);
    assert(config->firmware_patch == 0U);
    assert(config->startup_mode ==
           (s_test.factory_reset_marker_durable ?
            DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED :
            DEVICE_LINK_SERVICE_STARTUP_NORMAL));
    s_test.device_link_started_gated = config->startup_mode ==
                                       DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED;
    return _test_result(TEST_EVENT_BLE_INIT);
}

esp_err_t device_link_service_release_startup_gate(void)
{
    assert(s_test.device_link_started_gated);
    assert(!s_test.factory_reset_marker_durable);
    _test_record(TEST_EVENT_DEVICE_LINK_RELEASE_GATE);
    return ESP_OK;
}

esp_err_t device_link_service_deinit(uint32_t timeout_ms)
{
    assert(timeout_ms == DEVICE_LINK_SERVICE_WAIT_FOREVER);
    return _test_result(TEST_EVENT_BLE_DEINIT);
}

void app_runtime_pm_set_device_link_participant(bool enabled)
{
    s_test.device_link_participant = enabled;
}

esp_err_t factory_reset_service_init(
    const factory_reset_service_config_t *config)
{
    assert(config != NULL);
    assert(config->restart != NULL);
    return _test_result(TEST_EVENT_FACTORY_RESET_INIT);
}

esp_err_t factory_reset_service_deinit(void)
{
    return _test_result(TEST_EVENT_FACTORY_RESET_DEINIT);
}

esp_err_t factory_reset_service_recovery_pending(bool *pending)
{
    assert(pending != NULL);
    const esp_err_t result = _test_result(
                                 TEST_EVENT_FACTORY_RESET_RECOVERY_PENDING);

    if (result == ESP_OK)
    {
        *pending = s_test.factory_reset_marker_durable;
    }
    return result;
}

esp_err_t factory_reset_service_complete_recovery(void)
{
    assert(s_test.factory_reset_marker_durable);
    const esp_err_t result = _test_result(TEST_EVENT_FACTORY_RESET_COMPLETE);

    if (result == ESP_OK)
    {
        s_test.factory_reset_marker_durable = false;
    }
    return result;
}

static void _test_successful_lifecycle(void)
{
    static const test_event_t expected[] =
    {
        TEST_EVENT_LOG_INIT,
        TEST_EVENT_NV_INIT,
        TEST_EVENT_FACTORY_RESET_INIT,
        TEST_EVENT_FACTORY_RESET_RECOVERY_PENDING,
        TEST_EVENT_FS_INIT,
        TEST_EVENT_EVENT_BUS_INIT,
        TEST_EVENT_BLE_INIT,
        TEST_EVENT_BSP_INIT,
        TEST_EVENT_TIME_REGISTER_RTC,
        TEST_EVENT_TIME_INIT,
        TEST_EVENT_CHORE_INIT,
        TEST_EVENT_PM_BUILD_CONFIG,
        TEST_EVENT_SYSTEM_PM_INIT,
        TEST_EVENT_APP_MANAGER_INIT,
        TEST_EVENT_GET_UI_DISPATCH,
        TEST_EVENT_REGISTER_UI_DISPATCH,
        TEST_EVENT_REGISTER_WAKE_REQUESTER,
        TEST_EVENT_PM_PREPARE_POWER,
        TEST_EVENT_POWER_INIT,
        TEST_EVENT_IMU_REGISTER,
        TEST_EVENT_IMU_INIT,
        TEST_EVENT_AUDIO_INIT,
        TEST_EVENT_SD_REGISTER,
        TEST_EVENT_SD_INIT,
        TEST_EVENT_BUILTIN_DISCOVER,
        TEST_EVENT_APP_NAVIGATE,
        TEST_EVENT_DISPLAY_COMMIT,
        TEST_EVENT_SCREEN_COMMIT,
        TEST_EVENT_NETWORK_INIT,
        TEST_EVENT_CONNECTIVITY_INIT,
        TEST_EVENT_CONNECTIVITY_SUBSCRIBE,
        TEST_EVENT_CONNECTIVITY_GET_STATUS,
        TEST_EVENT_TIME_NETWORK_READY,
        TEST_EVENT_STARTUP_COMMIT,
        TEST_EVENT_DISPLAY_BENCHMARK_START,
        TEST_EVENT_TIME_NETWORK_READY,
        TEST_EVENT_TIME_NETWORK_READY,
        TEST_EVENT_TIME_NETWORK_READY,
        TEST_EVENT_DISPLAY_BENCHMARK_STOP,
        TEST_EVENT_SYSTEM_PM_CANCEL,
        TEST_EVENT_CONNECTIVITY_DEINIT,
        TEST_EVENT_TIME_NETWORK_READY,
        TEST_EVENT_CONNECTIVITY_UNSUBSCRIBE,
        TEST_EVENT_SD_DEINIT,
        TEST_EVENT_AUDIO_DEINIT,
        TEST_EVENT_IMU_DEINIT,
        TEST_EVENT_POWER_DEINIT,
        TEST_EVENT_UNREGISTER_WAKE_REQUESTER,
        TEST_EVENT_APP_MANAGER_DEINIT,
        TEST_EVENT_BLE_DEINIT,
        TEST_EVENT_UNREGISTER_UI_DISPATCH,
        TEST_EVENT_SYSTEM_PM_DEINIT,
        TEST_EVENT_CHORE_DEINIT,
        TEST_EVENT_TIME_DEINIT,
        TEST_EVENT_BSP_DEINIT,
        TEST_EVENT_FS_DEINIT,
        TEST_EVENT_FACTORY_RESET_DEINIT,
        TEST_EVENT_NV_DEINIT,
    };

    _test_reset();
    assert(app_runtime_start() == ESP_OK);
    assert(app_runtime_is_running());
    assert(s_test.connectivity_participant);
    assert(s_test.device_link_participant);
    assert(s_test.imu_participant);
    assert(s_test.audio_participant);
    assert(s_test.time_participant);
    assert(s_test.weather_participant);
    assert(s_test.time_network_ready);
    assert(s_test.weather_network_ready);
    assert(s_test.weather_ipv4_address == UINT32_C(0x0102a8c0));
    assert(s_test.imu_sample_rate_hz == 100U);
    assert(s_test.connectivity_callback != NULL);
    connectivity_manager_status_snapshot_t status =
    {
        .state = CONNECTIVITY_MANAGER_STATE_IDLE,
    };
    s_test.connectivity_callback(
        CONNECTIVITY_MANAGER_MSG,
        CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
        &status, sizeof(status), s_test.connectivity_callback_context);
    assert(!s_test.time_network_ready);
    assert(!s_test.weather_network_ready);
    status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    status.ipv4_address = UINT32_C(0x0102a8c0);
    s_test.connectivity_callback(
        CONNECTIVITY_MANAGER_MSG,
        CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
        &status, sizeof(status), s_test.connectivity_callback_context);
    assert(s_test.time_network_ready);
    assert(s_test.weather_network_ready);
    status.state = CONNECTIVITY_MANAGER_STATE_SCANNING;
    s_test.connectivity_callback(
        CONNECTIVITY_MANAGER_MSG,
        CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
        &status, sizeof(status), s_test.connectivity_callback_context);
    assert(s_test.time_network_ready);
    size_t event_count = s_test.event_count;
    assert(app_runtime_start() == ESP_OK);
    assert(s_test.event_count == event_count);
    assert(app_runtime_stop() == ESP_OK);
    assert(!app_runtime_is_running());
    assert(!s_test.connectivity_participant);
    assert(!s_test.device_link_participant);
    assert(!s_test.imu_participant);
    assert(!s_test.audio_participant);
    assert(!s_test.time_participant);
    assert(!s_test.weather_participant);
    assert(!s_test.time_network_ready);
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
        TEST_EVENT_FACTORY_RESET_INIT,
        TEST_EVENT_FACTORY_RESET_RECOVERY_PENDING,
        TEST_EVENT_FS_INIT,
        TEST_EVENT_EVENT_BUS_INIT,
        TEST_EVENT_BSP_INIT,
        TEST_EVENT_TIME_REGISTER_RTC,
        TEST_EVENT_TIME_INIT,
        TEST_EVENT_CHORE_INIT,
        TEST_EVENT_PM_BUILD_CONFIG,
        TEST_EVENT_SYSTEM_PM_INIT,
        TEST_EVENT_APP_MANAGER_INIT,
        TEST_EVENT_GET_UI_DISPATCH,
        TEST_EVENT_REGISTER_UI_DISPATCH,
        TEST_EVENT_REGISTER_WAKE_REQUESTER,
        TEST_EVENT_PM_PREPARE_POWER,
        TEST_EVENT_POWER_INIT,
        TEST_EVENT_IMU_REGISTER,
        TEST_EVENT_SD_REGISTER,
        TEST_EVENT_CONNECTIVITY_SUBSCRIBE,
        TEST_EVENT_CONNECTIVITY_GET_STATUS,
        TEST_EVENT_BLE_INIT,
        TEST_EVENT_APP_NAVIGATE,
        TEST_EVENT_DISPLAY_COMMIT,
        TEST_EVENT_SCREEN_COMMIT,
        TEST_EVENT_STARTUP_COMMIT,
        TEST_EVENT_DISPLAY_BENCHMARK_START,
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

static void _test_factory_reset_recovery_order(void)
{
    _test_reset();
    s_test.factory_reset_marker_durable = true;
    assert(app_runtime_start() == ESP_OK);
    assert(app_runtime_is_running());
    assert(_test_event_seen(TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE));
    assert(_test_event_seen(TEST_EVENT_FACTORY_RESET_COMPLETE));
    assert(_test_event_seen(TEST_EVENT_DEVICE_LINK_RELEASE_GATE));
    assert(_test_event_index(TEST_EVENT_FACTORY_RESET_RECOVERY_PENDING) <
           _test_event_index(TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE));
    assert(_test_event_index(TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE) <
           _test_event_index(TEST_EVENT_BLE_INIT));
    assert(_test_event_index(TEST_EVENT_BLE_INIT) <
           _test_event_index(TEST_EVENT_FACTORY_RESET_COMPLETE));
    assert(_test_event_index(TEST_EVENT_FACTORY_RESET_COMPLETE) <
           _test_event_index(TEST_EVENT_DEVICE_LINK_RELEASE_GATE));
    assert(_test_event_index(TEST_EVENT_DEVICE_LINK_RELEASE_GATE) <
           _test_event_index(TEST_EVENT_BSP_INIT));
    assert(_test_event_index(TEST_EVENT_BSP_INIT) <
           _test_event_index(TEST_EVENT_NETWORK_INIT));
    assert(!s_test.factory_reset_marker_durable);
    assert(app_runtime_stop() == ESP_OK);

    static const test_event_t fail_closed_boundaries[] =
    {
        TEST_EVENT_FACTORY_RESET_INIT,
        TEST_EVENT_FACTORY_RESET_RECOVERY_PENDING,
        TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE,
        TEST_EVENT_BLE_INIT,
        TEST_EVENT_FACTORY_RESET_COMPLETE,
    };

    for (size_t index = 0;
            index < sizeof(fail_closed_boundaries) /
            sizeof(fail_closed_boundaries[0]); ++index)
    {
        _test_reset();
        s_test.factory_reset_marker_durable = true;
        s_test.failure_event = fail_closed_boundaries[index];
        assert(app_runtime_start() == ESP_FAIL);
        assert(!app_runtime_is_running());
        assert(!_test_event_seen(TEST_EVENT_BSP_INIT));
        assert(!_test_event_seen(TEST_EVENT_NETWORK_INIT));
        assert(!_test_event_seen(TEST_EVENT_CONNECTIVITY_INIT));
        if (fail_closed_boundaries[index] ==
                TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE)
        {
            assert(!_test_event_seen(TEST_EVENT_BLE_INIT));
        }
        if (fail_closed_boundaries[index] ==
                TEST_EVENT_FACTORY_RESET_COMPLETE)
        {
            assert(!_test_event_seen(TEST_EVENT_DEVICE_LINK_RELEASE_GATE));
        }
        assert(s_test.factory_reset_marker_durable);
        assert(app_runtime_stop() == ESP_OK);

        _test_clear_events();
        s_test.failure_event = TEST_EVENT_NONE;
        s_test.device_link_started_gated = false;
        assert(app_runtime_start() == ESP_OK);
        assert(_test_event_seen(TEST_EVENT_CONNECTIVITY_CLEAR_PROFILE));
        assert(_test_event_seen(TEST_EVENT_FACTORY_RESET_COMPLETE));
        assert(_test_event_seen(TEST_EVENT_DEVICE_LINK_RELEASE_GATE));
        assert(!s_test.factory_reset_marker_durable);
        assert(app_runtime_stop() == ESP_OK);
    }

}

static void _test_cleanup_retry_before_restart(void)
{
    static const test_event_t first_cleanup[] =
    {
        TEST_EVENT_DISPLAY_BENCHMARK_STOP,
        TEST_EVENT_SYSTEM_PM_CANCEL,
        TEST_EVENT_CONNECTIVITY_DEINIT,
    };

    _test_reset();
    assert(app_runtime_start() == ESP_OK);
    _test_clear_events();
    s_test.failure_event = TEST_EVENT_CONNECTIVITY_DEINIT;
    assert(app_runtime_stop() == ESP_FAIL);
    assert(!app_runtime_is_running());
    _test_expect_events(first_cleanup,
                        sizeof(first_cleanup) / sizeof(first_cleanup[0]));

    _test_clear_events();
    s_test.failure_event = TEST_EVENT_NONE;
    assert(app_runtime_start() == ESP_OK);
    assert(app_runtime_is_running());
    assert(s_test.events[0] == TEST_EVENT_CONNECTIVITY_DEINIT);
    /* The first attempt failed before the App Manager stage, so the retry
     * completes it, including the Device Link teardown that now runs after
     * app_manager_deinit. */
    assert(_test_event_seen(TEST_EVENT_BLE_DEINIT));
    assert(_test_event_seen(TEST_EVENT_LOG_INIT));
    assert(app_runtime_stop() == ESP_OK);
}

static void _test_every_cleanup_failure_is_retryable(void)
{
    static const test_event_t cleanup_events[] =
    {
        TEST_EVENT_DISPLAY_BENCHMARK_STOP,
        TEST_EVENT_SYSTEM_PM_CANCEL,
        TEST_EVENT_CONNECTIVITY_DEINIT,
        TEST_EVENT_TIME_NETWORK_READY,
        TEST_EVENT_CONNECTIVITY_UNSUBSCRIBE,
        TEST_EVENT_SD_DEINIT,
        TEST_EVENT_AUDIO_DEINIT,
        TEST_EVENT_IMU_DEINIT,
        TEST_EVENT_POWER_DEINIT,
        TEST_EVENT_UNREGISTER_WAKE_REQUESTER,
        TEST_EVENT_APP_MANAGER_DEINIT,
        TEST_EVENT_BLE_DEINIT,
        TEST_EVENT_UNREGISTER_UI_DISPATCH,
        TEST_EVENT_SYSTEM_PM_DEINIT,
        TEST_EVENT_CHORE_DEINIT,
        TEST_EVENT_TIME_DEINIT,
        TEST_EVENT_BSP_DEINIT,
        TEST_EVENT_FS_DEINIT,
        TEST_EVENT_FACTORY_RESET_DEINIT,
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
        if (cleanup_events[index] != TEST_EVENT_SYSTEM_PM_CANCEL &&
                cleanup_events[index] != TEST_EVENT_DISPLAY_BENCHMARK_STOP)
        {
            assert(!_test_event_seen(TEST_EVENT_SYSTEM_PM_CANCEL));
        }
    }
}

static void _test_degradable_hardware_failures(void)
{
    static const struct
    {
        test_event_t init_event;
        test_event_t deinit_event;
    } cases[] =
    {
        {TEST_EVENT_IMU_INIT, TEST_EVENT_IMU_DEINIT},
        {TEST_EVENT_AUDIO_INIT, TEST_EVENT_AUDIO_DEINIT},
        {TEST_EVENT_SD_INIT, TEST_EVENT_SD_DEINIT},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    {
        _test_reset();
        s_test.failure_event = cases[index].init_event;
        assert(app_runtime_start() == ESP_OK);
        assert(app_runtime_is_running());
        assert(_test_event_seen(cases[index].deinit_event));

        _test_clear_events();
        s_test.failure_event = TEST_EVENT_NONE;
        assert(app_runtime_stop() == ESP_OK);
        assert(!_test_event_seen(cases[index].deinit_event));
    }
}

static void _test_imu_init_cleanup_retry_keeps_bridge(void)
{
    _test_reset();
    s_test.failure_event = TEST_EVENT_IMU_INIT;
    s_test.secondary_failure_event = TEST_EVENT_IMU_DEINIT;
    assert(app_runtime_start() == ESP_FAIL);
    assert(!app_runtime_is_running());
    assert(_test_event_seen(TEST_EVENT_IMU_DEINIT));

    _test_clear_events();
    s_test.secondary_failure_event = TEST_EVENT_NONE;
    assert(app_runtime_stop() == ESP_OK);
    assert(!app_runtime_is_running());
    assert(_test_event_seen(TEST_EVENT_IMU_DEINIT));
}

static void _test_sd_mount_error_exposes_cleanup_handle(void)
{
    _test_reset();
    assert(app_runtime_start() == ESP_OK);
    assert(s_test.sd_mount_ops.mount != NULL);
    assert(s_test.sd_mount_ops.unmount != NULL);
    assert(s_test.sd_mount_ops.is_mounted != NULL);

    const sd_storage_service_config_t config =
    {
        .mount_path = "/sdcard",
        .max_files = 5,
        .allocation_unit_size = 16U * 1024U,
    };
    s_test.sd_mount_result = ESP_FAIL;
    s_test.sd_mounted = false;
    void *handle = NULL;
    assert(s_test.sd_mount_ops.mount(s_test.sd_mount_ops.context,
                                     &config,
                                     SD_STORAGE_SERVICE_MOUNT_NORMAL,
                                     &handle) == ESP_FAIL);
    assert(handle == &s_sd_ops);
    assert(!s_test.sd_mount_ops.is_mounted(s_test.sd_mount_ops.context,
                                           handle));
    assert(s_test.sd_mount_ops.unmount(s_test.sd_mount_ops.context,
                                       handle) == ESP_OK);
    assert(app_runtime_stop() == ESP_OK);
}

static void _test_degradable_connectivity_failures(void)
{
    _test_reset();
    s_test.failure_event = TEST_EVENT_NETWORK_INIT;
    s_test.network_ready = false;
    assert(app_runtime_start() == ESP_OK);
    assert(!_test_event_seen(TEST_EVENT_CONNECTIVITY_INIT));
    assert(_test_event_seen(TEST_EVENT_BLE_INIT));
    _test_clear_events();
    assert(app_runtime_stop() == ESP_OK);
    assert(!_test_event_seen(TEST_EVENT_CONNECTIVITY_DEINIT));

    _test_reset();
    s_test.failure_event = TEST_EVENT_CONNECTIVITY_INIT;
    assert(app_runtime_start() == ESP_OK);
    assert(_test_event_seen(TEST_EVENT_BLE_INIT));
    _test_clear_events();
    assert(app_runtime_stop() == ESP_OK);
    assert(!_test_event_seen(TEST_EVENT_CONNECTIVITY_DEINIT));
}

int main(void)
{
    _test_successful_lifecycle();
    _test_fatal_start_failures();
    _test_factory_reset_recovery_order();
    _test_cleanup_retry_before_restart();
    _test_every_cleanup_failure_is_retryable();
    _test_degradable_connectivity_failures();
    _test_degradable_hardware_failures();
    _test_imu_init_cleanup_retry_keeps_bridge();
    _test_sd_mount_error_exposes_cleanup_handle();
    return 0;
}

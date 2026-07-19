#define DBG_TAG "app_runtime"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_runtime.h"
#include "app_runtime_pm.h"

#include "app_manager.h"
#include "app_manager_config.h"
#include "audio_service.h"
#include "ble_service.h"
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
#include "wifi_service.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum
{
    APP_RUNTIME_STOPPED = 0,
    APP_RUNTIME_STARTING,
    APP_RUNTIME_READY,
    APP_RUNTIME_STOPPING,
    APP_RUNTIME_CLEANUP_PENDING,
} app_runtime_state_t;

typedef struct app_runtime_ownership
{
    bool nv_attempted;
    bool fs_attempted;
    bool bsp_attempted;
    bool time_attempted;
    bool system_pm_attempted;
    bool system_pm_cancelable;
    bool app_manager_attempted;
    bool ui_dispatch_registered;
    bool wake_requester_registered;
    bool power_attempted;
    bool imu_attempted;
    bool audio_attempted;
    bool sd_attempted;
    bool wifi_owned;
    bool ble_attempted;
    app_manager_ui_dispatch_fn ui_dispatch;
} app_runtime_ownership_t;

typedef struct app_runtime_start_context
{
    bsp_capabilities_t capabilities;
    const bsp_screen_ops_t *screen;
    const bsp_display_port_t *display;
} app_runtime_start_context_t;

static atomic_int s_runtime_state = ATOMIC_VAR_INIT(APP_RUNTIME_STOPPED);
static app_runtime_ownership_t s_ownership;
static const bsp_rtc_ops_t *s_runtime_rtc;
static const bsp_imu_ops_t *s_runtime_imu;
static const bsp_sd_ops_t *s_runtime_sd;

static void _app_runtime_record_first_error(esp_err_t *first_error,
        esp_err_t result)
{
    if (*first_error == ESP_OK && result != ESP_OK)
    {
        *first_error = result;
    }
}

static esp_err_t _app_runtime_rtc_alarm_configure(
    const time_service_alarm_config_t *config)
{
    if (config == NULL || s_runtime_rtc == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const bsp_rtc_alarm_config_t board_config =
    {
        .match_second = config->match_second,
        .second = config->second,
        .match_minute = config->match_minute,
        .minute = config->minute,
        .match_hour = config->match_hour,
        .hour = config->hour,
        .match_day = config->match_day,
        .day = config->day,
        .match_weekday = config->match_weekday,
        .weekday = config->weekday,
    };
    return s_runtime_rtc->alarm_configure(&board_config);
}

static esp_err_t _app_runtime_rtc_alarm_disable(void)
{
    return s_runtime_rtc == NULL ? ESP_ERR_INVALID_STATE :
           s_runtime_rtc->alarm_disable();
}

static esp_err_t _app_runtime_rtc_alarm_get_status(
    time_service_alarm_status_t *status)
{
    if (status == NULL || s_runtime_rtc == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_rtc_alarm_status_t board_status;
    esp_err_t result = s_runtime_rtc->alarm_get_status(&board_status);
    if (result == ESP_OK)
    {
        *status = (time_service_alarm_status_t)
        {
            .enabled = board_status.enabled,
            .pending = board_status.pending,
            .interrupt_active = false,
        };
    }
    return result;
}

static esp_err_t _app_runtime_rtc_alarm_clear(void)
{
    return s_runtime_rtc == NULL ? ESP_ERR_INVALID_STATE :
           s_runtime_rtc->alarm_clear();
}

static esp_err_t _app_runtime_rtc_alarm_poll_interrupt(bool *active)
{
    if (active == NULL || s_runtime_rtc == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_runtime_rtc->alarm_poll_interrupt(active);
}

static bool _app_runtime_imu_is_available(void)
{
    return s_runtime_imu != NULL && s_runtime_imu->is_available();
}

static esp_err_t _app_runtime_imu_configure(uint32_t sample_rate_hz)
{
    return s_runtime_imu == NULL ? ESP_ERR_INVALID_STATE :
           s_runtime_imu->configure(sample_rate_hz);
}

static esp_err_t _app_runtime_imu_read(imu_service_sample_t *sample)
{
    if (sample == NULL || s_runtime_imu == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_imu_sample_t board_sample = {0};
    esp_err_t result = s_runtime_imu->read(&board_sample);
    if (result != ESP_OK)
    {
        return result;
    }

    sample->acceleration_mps2.x = board_sample.accel_mps2[0];
    sample->acceleration_mps2.y = board_sample.accel_mps2[1];
    sample->acceleration_mps2.z = board_sample.accel_mps2[2];
    sample->angular_velocity_dps.x = board_sample.gyro_dps[0];
    sample->angular_velocity_dps.y = board_sample.gyro_dps[1];
    sample->angular_velocity_dps.z = board_sample.gyro_dps[2];
    sample->temperature_c = board_sample.temperature_c;
    sample->sensor_timestamp = board_sample.sensor_timestamp;
    sample->status_int = board_sample.status_int;
    sample->status0 = board_sample.status0;
    sample->status1 = board_sample.status1;
    sample->data_ready = board_sample.data_ready;
    sample->interrupt_active = board_sample.interrupt_active;
    sample->interrupt_level_valid = board_sample.interrupt_level_valid;
    return ESP_OK;
}

static esp_err_t _app_runtime_imu_set_enabled(bool enabled)
{
    return s_runtime_imu == NULL ? ESP_ERR_INVALID_STATE :
           s_runtime_imu->set_enabled(enabled);
}

static esp_err_t _app_runtime_imu_poll_interrupt(bool *active)
{
    if (active == NULL || s_runtime_imu == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_runtime_imu->get_interrupt_level(active);
}

static esp_err_t _app_runtime_sd_mount(
    void *context, const sd_storage_service_config_t *config,
    void **out_handle)
{
    const bsp_sd_ops_t *ops = context;
    if (ops == NULL || config == NULL || out_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const bsp_sd_config_t board_config =
    {
        .mount_point = config->mount_path,
        .format_if_mount_failed = config->format_if_mount_failed,
        .max_files = config->max_files,
        .allocation_unit_size = config->allocation_unit_size,
    };
    *out_handle = (void *)ops;
    return ops->mount(&board_config);
}

static esp_err_t _app_runtime_sd_unmount(void *context, void *handle)
{
    const bsp_sd_ops_t *ops = context;
    if (ops == NULL || handle != ops)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ops->unmount();
}

static bool _app_runtime_sd_is_mounted(void *context, void *handle)
{
    const bsp_sd_ops_t *ops = context;
    return ops != NULL && handle == ops && ops->is_mounted();
}

static bool _app_runtime_has_owned_resources(void)
{
    return s_ownership.nv_attempted || s_ownership.fs_attempted ||
           s_ownership.bsp_attempted || s_ownership.time_attempted ||
           s_ownership.system_pm_attempted ||
           s_ownership.app_manager_attempted ||
           s_ownership.ui_dispatch_registered ||
           s_ownership.wake_requester_registered ||
           s_ownership.power_attempted || s_ownership.imu_attempted ||
           s_ownership.audio_attempted || s_ownership.sd_attempted ||
           s_ownership.wifi_owned ||
           s_ownership.ble_attempted;
}

static bool _app_runtime_required_apps_present(void)
{
    static const char *const required_ids[] =
    {
        APP_MANAGER_ID_HOME,
        APP_MANAGER_ID_MENU,
        APP_MANAGER_ID_SETTINGS,
        APP_MANAGER_ID_SETUP,
    };
    bool found[sizeof(required_ids) / sizeof(required_ids[0])] = {false};
    bool all_found = true;
    const app_manager_app_desc_t *app = app_manager_builtin_list_open();
    while (app != NULL)
    {
        for (size_t i = 0; i < sizeof(required_ids) / sizeof(required_ids[0]); ++i)
        {
            if (app->id != NULL && strcmp(app->id, required_ids[i]) == 0)
            {
                found[i] = app->root_page_id != NULL;
            }
        }
        app = app_manager_builtin_list_get_next(app);
    }
    for (size_t i = 0; i < sizeof(found) / sizeof(found[0]); ++i)
    {
        if (!found[i])
        {
            all_found = false;
            break;
        }
    }
    return all_found;
}

static esp_err_t _app_runtime_unregister_wake_requester(void)
{
    esp_err_t result = event_bus_unregister_wake_requester(
                           app_manager_pm_notify_user_activity);
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
    {
        s_ownership.wake_requester_registered = false;
        result = ESP_OK;
    }
    return result;
}

static esp_err_t _app_runtime_unregister_ui_dispatch(void)
{
    esp_err_t result = event_bus_unregister_ui_dispatch(
                           s_ownership.ui_dispatch);
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
    {
        s_ownership.ui_dispatch_registered = false;
        s_ownership.ui_dispatch = NULL;
        result = ESP_OK;
    }
    return result;
}

static esp_err_t _app_runtime_stop_active_services(void)
{
    esp_err_t result = ESP_OK;
    if (s_ownership.system_pm_cancelable)
    {
        result = system_pm_cancel_standby();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.system_pm_cancelable = false;
    }
    if (s_ownership.ble_attempted)
    {
        result = ble_service_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.ble_attempted = false;
    }
    if (s_ownership.wifi_owned)
    {
        result = wifi_service_deinit(WIFI_SERVICE_WAIT_FOREVER);
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.wifi_owned = false;
        app_runtime_pm_set_wifi_participant(false);
    }
    if (s_ownership.sd_attempted)
    {
        result = sd_storage_service_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.sd_attempted = false;
        s_runtime_sd = NULL;
    }
    if (s_ownership.audio_attempted)
    {
        result = audio_service_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.audio_attempted = false;
        app_runtime_pm_set_audio_participant(false);
    }
    if (s_ownership.imu_attempted)
    {
        result = imu_service_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.imu_attempted = false;
        s_runtime_imu = NULL;
        app_runtime_pm_set_imu_participant(false);
    }
    if (s_ownership.power_attempted)
    {
        result = power_service_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.power_attempted = false;
        app_runtime_pm_clear_power();
    }
    return result;
}

static esp_err_t _app_runtime_stop_app_services(void)
{
    esp_err_t result = ESP_OK;
    if (s_ownership.wake_requester_registered)
    {
        result = _app_runtime_unregister_wake_requester();
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (s_ownership.app_manager_attempted)
    {
        result = app_manager_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.app_manager_attempted = false;
    }
    if (s_ownership.ui_dispatch_registered)
    {
        result = _app_runtime_unregister_ui_dispatch();
        if (result != ESP_OK)
        {
            return result;
        }
    }
    return result;
}

static esp_err_t _app_runtime_stop_platform_services(void)
{
    esp_err_t result = ESP_OK;
    if (s_ownership.system_pm_attempted)
    {
        s_ownership.system_pm_cancelable = false;
        result = system_pm_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.system_pm_attempted = false;
    }
    if (s_ownership.time_attempted)
    {
        result = time_service_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.time_attempted = false;
        s_runtime_rtc = NULL;
        app_runtime_pm_set_time_participant(false);
    }
    return result;
}

static esp_err_t _app_runtime_stop_foundations(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_ownership.bsp_attempted)
    {
        esp_err_t result = bsp_deinit();
        _app_runtime_record_first_error(&first_error, result);
        if (result == ESP_OK)
        {
            s_ownership.bsp_attempted = false;
        }
        app_runtime_pm_detach_bsp();
    }
    if (s_ownership.fs_attempted)
    {
        esp_err_t result = fs_storage_deinit();
        _app_runtime_record_first_error(&first_error, result);
        if (result == ESP_OK)
        {
            s_ownership.fs_attempted = false;
        }
    }
    if (s_ownership.nv_attempted)
    {
        esp_err_t result = nv_storage_deinit();
        _app_runtime_record_first_error(&first_error, result);
        if (result == ESP_OK)
        {
            s_ownership.nv_attempted = false;
        }
    }

    return first_error;
}

static esp_err_t _app_runtime_unwind(void)
{
    atomic_store(&s_runtime_state, APP_RUNTIME_STOPPING);
    app_runtime_pm_close_admission();

    esp_err_t result = _app_runtime_stop_active_services();
    if (result != ESP_OK)
    {
        goto blocked;
    }
    result = _app_runtime_stop_app_services();
    if (result != ESP_OK)
    {
        goto blocked;
    }
    result = _app_runtime_stop_platform_services();
    if (result != ESP_OK)
    {
        goto blocked;
    }
    result = _app_runtime_stop_foundations();
    if (!_app_runtime_has_owned_resources())
    {
        memset(&s_ownership, 0, sizeof(s_ownership));
        app_runtime_pm_reset();
        atomic_store(&s_runtime_state, APP_RUNTIME_STOPPED);
        return result;
    }
    if (result == ESP_OK)
    {
        result = ESP_ERR_INVALID_STATE;
    }

blocked:
    atomic_store(&s_runtime_state, APP_RUNTIME_CLEANUP_PENDING);
    return result;
}

static esp_err_t _app_runtime_start_foundations(void)
{
    esp_err_t result = mt_log_init();
    if (result != ESP_OK)
    {
        return result;
    }
    LOG_I("microtech connectivity runtime startup");

    s_ownership.nv_attempted = true;
    result = nv_storage_init();
    if (result != ESP_OK)
    {
        return result;
    }

    s_ownership.fs_attempted = true;
    result = fs_storage_init();
    if (result != ESP_OK)
    {
        return result;
    }

    result = event_bus_init();
    return result;
}

static esp_err_t _app_runtime_start_platform(
    app_runtime_start_context_t *context)
{
    esp_err_t result = ESP_OK;
    s_ownership.bsp_attempted = true;
    result = bsp_init();
    if (result != ESP_OK)
    {
        return result;
    }

    context->capabilities = bsp_get_capabilities();
    LOG_I("BSP ready, capabilities=0x%08x", (unsigned)context->capabilities);
    const bsp_capabilities_t required_capabilities =
        BSP_CAPABILITY_DISPLAY | BSP_CAPABILITY_TOUCH | BSP_CAPABILITY_INPUT;
    if ((context->capabilities & required_capabilities) != required_capabilities)
    {
        result = ESP_ERR_INVALID_STATE;
        return result;
    }

    s_ownership.time_attempted = true;
    if ((context->capabilities & BSP_CAPABILITY_RTC) != 0)
    {
        const bsp_rtc_ops_t *rtc = bsp_hal_get_rtc();
        if (rtc == NULL)
        {
            result = ESP_ERR_INVALID_STATE;
            return result;
        }
        s_runtime_rtc = rtc;
        const time_service_rtc_ops_t rtc_ops =
        {
            .is_available = rtc->is_available,
            .read = rtc->read,
            .write = rtc->write,
            .alarm_configure = _app_runtime_rtc_alarm_configure,
            .alarm_disable = _app_runtime_rtc_alarm_disable,
            .alarm_get_status = _app_runtime_rtc_alarm_get_status,
            .alarm_clear = _app_runtime_rtc_alarm_clear,
            .alarm_poll_interrupt = _app_runtime_rtc_alarm_poll_interrupt,
        };
        result = time_service_register_rtc_ops(&rtc_ops);
        if (result != ESP_OK)
        {
            s_runtime_rtc = NULL;
            return result;
        }
    }
    else
    {
        LOG_W("RTC unavailable; time quality starts INVALID");
    }
    result = time_service_init();
    if (result != ESP_OK)
    {
        return result;
    }
    app_runtime_pm_set_time_participant(true);

    context->screen = bsp_hal_get_screen();
    context->display = bsp_display_get_port();
    if (context->screen == NULL || context->display == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        return result;
    }

    system_pm_config_t system_pm_config;
    result = app_runtime_pm_build_system_config(&system_pm_config);
    if (result != ESP_OK)
    {
        return result;
    }
    s_ownership.system_pm_attempted = true;
    result = system_pm_init(&system_pm_config);
    if (result == ESP_OK)
    {
        s_ownership.system_pm_cancelable = true;
    }
    return result;
}

static esp_err_t _app_runtime_start_app_services(
    const app_runtime_start_context_t *context)
{
    static const uint16_t font_sizes[] = APP_THEME_FONT_SIZES;
    const app_manager_config_t app_config =
    {
        .disp_port = context->display,
        .font_sizes = font_sizes,
        .font_count = sizeof(font_sizes) / sizeof(font_sizes[0]),
        .res_fs_letter = 'F',
        .screen_ops = {
            .suspend = context->screen->suspend,
            .resume_prepare = context->screen->resume_prepare,
            .resume_commit = context->screen->resume_commit,
            .is_suspended = context->screen->is_suspended,
            .is_suspend_committed = context->screen->is_suspend_committed,
            .set_brightness = context->screen->set_brightness,
            .set_brightness_temp = context->screen->set_brightness_temp,
            .get_brightness = context->screen->get_brightness,
        },
        .input_ops = app_runtime_pm_get_input_ops(),
        .standby_ops = app_runtime_pm_get_standby_ops(),
        .page_memory_bytes = 32U * 1024U,
        .max_resident_apps = APP_MANAGER_MAX_RESIDENT_APPS,
        .resident_policy = APP_MANAGER_RESIDENT_REJECT,
        .app_forward_transition = {
            .effect = APP_MANAGER_TRANSITION_FADE,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .app_back_transition = {
            .effect = APP_MANAGER_TRANSITION_FADE,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .page_forward_transition = {
            .effect = APP_MANAGER_TRANSITION_PUSH_LEFT,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .page_back_transition = {
            .effect = APP_MANAGER_TRANSITION_PUSH_RIGHT,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
    };

    esp_err_t result = ESP_OK;
    s_ownership.app_manager_attempted = true;
    result = app_manager_init(&app_config);
    if (result != ESP_OK)
    {
        return result;
    }

    result = app_manager_get_ui_dispatch_fn(&s_ownership.ui_dispatch);
    if (result != ESP_OK)
    {
        return result;
    }
    result = event_bus_register_ui_dispatch(s_ownership.ui_dispatch);
    if (result != ESP_OK)
    {
        return result;
    }
    s_ownership.ui_dispatch_registered = true;

    result = event_bus_register_wake_requester(
                 app_manager_pm_notify_user_activity);
    if (result != ESP_OK)
    {
        return result;
    }
    s_ownership.wake_requester_registered = true;

    s_ownership.power_attempted = true;
    result = app_runtime_pm_prepare_power(context->capabilities);
    if (result != ESP_OK)
    {
        return result;
    }
    result = power_service_init();
    if (result != ESP_OK)
    {
        return result;
    }

    if ((context->capabilities & BSP_CAPABILITY_IMU) != 0)
    {
        s_runtime_imu = bsp_hal_get_imu();
        if (s_runtime_imu == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const imu_service_imu_ops_t imu_ops =
        {
            .is_available = _app_runtime_imu_is_available,
            .configure = _app_runtime_imu_configure,
            .read = _app_runtime_imu_read,
            .set_enabled = _app_runtime_imu_set_enabled,
            .poll_interrupt = _app_runtime_imu_poll_interrupt,
        };
        result = imu_service_register_imu_ops(&imu_ops);
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.imu_attempted = true;
        result = imu_service_init();
        if (result != ESP_OK)
        {
            const esp_err_t cleanup_result = imu_service_deinit();
            s_ownership.imu_attempted = cleanup_result != ESP_OK;
            if (cleanup_result == ESP_OK)
            {
                s_runtime_imu = NULL;
            }
            if (cleanup_result != ESP_OK)
            {
                return cleanup_result;
            }
            LOG_W("IMU service unavailable: %s", esp_err_to_name(result));
        }
        else
        {
            app_runtime_pm_set_imu_participant(true);
        }
    }

    if ((context->capabilities & BSP_CAPABILITY_AUDIO) != 0)
    {
        s_ownership.audio_attempted = true;
        result = audio_service_init();
        if (result != ESP_OK)
        {
            const esp_err_t cleanup_result = audio_service_deinit();
            s_ownership.audio_attempted = cleanup_result != ESP_OK;
            if (cleanup_result != ESP_OK)
            {
                return cleanup_result;
            }
            LOG_W("audio service unavailable: %s", esp_err_to_name(result));
        }
        else
        {
            app_runtime_pm_set_audio_participant(true);
        }
    }

    if ((context->capabilities & BSP_CAPABILITY_SD) != 0)
    {
        s_runtime_sd = bsp_hal_get_sd();
        if (s_runtime_sd == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const sd_storage_service_mount_ops_t sd_ops =
        {
            .context = (void *)s_runtime_sd,
            .mount = _app_runtime_sd_mount,
            .unmount = _app_runtime_sd_unmount,
            .is_mounted = _app_runtime_sd_is_mounted,
        };
        result = sd_storage_service_register_mount_ops(&sd_ops);
        if (result != ESP_OK)
        {
            return result;
        }
        s_ownership.sd_attempted = true;
        result = sd_storage_service_init();
        if (result != ESP_OK)
        {
            const esp_err_t cleanup_result = sd_storage_service_deinit();
            s_ownership.sd_attempted = cleanup_result != ESP_OK;
            s_runtime_sd = NULL;
            if (cleanup_result != ESP_OK)
            {
                return cleanup_result;
            }
            LOG_W("SD card unavailable: %s", esp_err_to_name(result));
        }
    }
    return ESP_OK;
}

static esp_err_t _app_runtime_start_connectivity(void)
{
    esp_err_t result = network_runtime_init();
    if (result != ESP_OK || !network_runtime_is_ready())
    {
        LOG_W("network foundation unavailable; continuing offline: %s",
              esp_err_to_name(result));
        result = ESP_OK;
    }
    else
    {
        result = wifi_service_init();
        if (result != ESP_OK)
        {
            if (wifi_service_is_cleanup_pending())
            {
                s_ownership.wifi_owned = true;
                app_runtime_pm_set_wifi_participant(true);
                return result;
            }
            LOG_W("WiFi unavailable; continuing offline: %s",
                  esp_err_to_name(result));
            result = ESP_OK;
        }
        else
        {
            s_ownership.wifi_owned = true;
            app_runtime_pm_set_wifi_participant(true);
        }
    }

    s_ownership.ble_attempted = true;
    result = ble_service_init();
    return result;
}

static esp_err_t _app_runtime_start_initial_app(void)
{
    esp_err_t result = ESP_OK;
    const int app_count = app_manager_builtin_discover();
    LOG_I("builtin apps discovered: %d", app_count);
    if (!_app_runtime_required_apps_present())
    {
        result = ESP_ERR_NOT_FOUND;
        return result;
    }

    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = APP_MANAGER_ID_HOME,
        .transition = {
            .effect = APP_MANAGER_TRANSITION_NONE,
        },
    };
    result = app_manager_navigate(&request, UINT32_MAX);
    if (result != ESP_OK)
    {
        return result;
    }

    /* The BSP keeps SH8601 hidden after cold init. Build and fence the first
     * LVGL frame before committing display-on and the physical touch IRQ. */
    result = app_manager_display_commit_initial();
    return result;
}

static esp_err_t _app_runtime_start_failed(esp_err_t primary_error)
{
    const esp_err_t cleanup_result = _app_runtime_unwind();
    if (cleanup_result != ESP_OK)
    {
        LOG_E("runtime cleanup pending: primary=%s cleanup=%s",
              esp_err_to_name(primary_error), esp_err_to_name(cleanup_result));
    }
    return primary_error;
}

esp_err_t app_runtime_start(void)
{
    esp_err_t result = ESP_OK;
    const app_runtime_state_t state =
        (app_runtime_state_t)atomic_load(&s_runtime_state);
    if (state == APP_RUNTIME_READY)
    {
        return result;
    }
    if (state == APP_RUNTIME_STARTING || state == APP_RUNTIME_STOPPING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == APP_RUNTIME_CLEANUP_PENDING)
    {
        result = app_runtime_stop();
        if (result != ESP_OK)
        {
            return result;
        }
    }

    atomic_store(&s_runtime_state, APP_RUNTIME_STARTING);
    app_runtime_pm_close_admission();
    memset(&s_ownership, 0, sizeof(s_ownership));
    s_runtime_rtc = NULL;
    s_runtime_imu = NULL;
    s_runtime_sd = NULL;
    app_runtime_pm_reset();

    app_runtime_start_context_t context = {0};
    result = _app_runtime_start_foundations();
    if (result != ESP_OK)
    {
        goto failed;
    }

    result = _app_runtime_start_platform(&context);
    if (result != ESP_OK)
    {
        goto failed;
    }

    result = _app_runtime_start_app_services(&context);
    if (result != ESP_OK)
    {
        goto failed;
    }

    result = _app_runtime_start_initial_app();
    if (result != ESP_OK)
    {
        goto failed;
    }

    result = _app_runtime_start_connectivity();
    if (result != ESP_OK)
    {
        goto failed;
    }

    result = app_manager_startup_commit();
    if (result != ESP_OK)
    {
        goto failed;
    }

    atomic_store(&s_runtime_state, APP_RUNTIME_READY);
    app_runtime_pm_open_admission();
    return result;

failed:
    return _app_runtime_start_failed(result);
}

esp_err_t app_runtime_stop(void)
{
    esp_err_t result = ESP_OK;
    const app_runtime_state_t state =
        (app_runtime_state_t)atomic_load(&s_runtime_state);
    if (state == APP_RUNTIME_STOPPED && !_app_runtime_has_owned_resources())
    {
        return result;
    }
    if (state == APP_RUNTIME_STARTING || state == APP_RUNTIME_STOPPING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    result = _app_runtime_unwind();
    return result;
}

bool app_runtime_is_running(void)
{
    return atomic_load(&s_runtime_state) == APP_RUNTIME_READY;
}

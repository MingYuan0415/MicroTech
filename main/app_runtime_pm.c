#define DBG_TAG "app_runtime_pm"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_runtime_pm.h"

#include "app_manager.h"
#include "power_service.h"
#include "wifi_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define APP_RUNTIME_SLEEP_TIMEOUT_MS 1000U

typedef struct app_runtime_sleep_context
{
    const bsp_input_ops_t *input;
    bool wifi_participant;
    bool wifi_resume_required;
    bool power_resume_required;
    bool input_resume_required;
} app_runtime_sleep_context_t;

static atomic_bool s_standby_admitted = ATOMIC_VAR_INIT(false);
static atomic_uint s_standby_request_users = ATOMIC_VAR_INIT(0U);
static app_runtime_sleep_context_t s_sleep_context;
static const bsp_power_ops_t *s_bsp_power;
static const bsp_input_ops_t *s_bsp_input;
static app_manager_input_cb_t s_app_input_callback;
static void *s_app_input_context;

static esp_err_t _app_runtime_pm_complete_sleep(uint32_t timeout_ms,
        void *context);

static void _app_runtime_pm_record_first_error(esp_err_t *first_error,
        esp_err_t result)
{
    if (*first_error == ESP_OK && result != ESP_OK)
    {
        *first_error = result;
    }
}

static bool _app_runtime_pm_commit_guard(uint32_t generation, void *context)
{
    (void)context;
    return app_manager_pm_standby_commit_guard(generation, NULL);
}

static void _app_runtime_pm_commit_callback(uint32_t generation, void *context)
{
    (void)context;
    app_manager_pm_standby_commit_callback(generation, NULL);
}

static bool _app_runtime_pm_standby_request_begin(void)
{
    bool admitted = atomic_load_explicit(&s_standby_admitted,
                                         memory_order_acquire);
    if (admitted)
    {
        atomic_fetch_add_explicit(&s_standby_request_users, 1U,
                                  memory_order_acq_rel);
        admitted = atomic_load_explicit(&s_standby_admitted,
                                        memory_order_acquire);
        if (!admitted)
        {
            atomic_fetch_sub_explicit(&s_standby_request_users, 1U,
                                      memory_order_release);
        }
    }
    return admitted;
}

static esp_err_t _app_runtime_pm_request_standby(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (_app_runtime_pm_standby_request_begin())
    {
        result = system_pm_request_standby();
        atomic_fetch_sub_explicit(&s_standby_request_users, 1U,
                                  memory_order_release);
    }
    return result;
}

static esp_err_t _app_runtime_pm_cancel_standby(void)
{
    return system_pm_cancel_standby();
}

static esp_err_t _app_runtime_pm_power_get_info(power_info_t *output)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (output == NULL || s_bsp_power == NULL || s_bsp_power->get_info == NULL)
    {
        return result;
    }

    bsp_power_info_t board_info;
    result = s_bsp_power->get_info(&board_info);
    if (result == ESP_OK)
    {
        output->battery_voltage_mv = board_info.battery_voltage_mv;
        output->battery_percent = board_info.battery_percent;
        output->is_charging = board_info.is_charging;
        output->is_vbus_connected = board_info.is_vbus_connected;
    }
    return result;
}

static void _app_runtime_pm_bsp_input_callback(bsp_key_t key,
        bsp_key_event_t event, void *user_data)
{
    (void)user_data;
    app_manager_key_t app_key;
    app_manager_key_event_t app_event;
    bool mapped = true;

    switch (key)
    {
    case BSP_KEY_HOME:
        app_key = APP_MANAGER_KEY_HOME;
        break;
    case BSP_KEY_POWER:
        app_key = APP_MANAGER_KEY_POWER;
        break;
    default:
        mapped = false;
        break;
    }

    if (mapped)
    {
        switch (event)
        {
        case BSP_KEY_EVENT_DOWN:
            app_event = APP_MANAGER_KEY_EVENT_DOWN;
            break;
        case BSP_KEY_EVENT_UP:
            app_event = APP_MANAGER_KEY_EVENT_UP;
            break;
        case BSP_KEY_EVENT_CLICK:
            app_event = APP_MANAGER_KEY_EVENT_CLICK;
            break;
        case BSP_KEY_EVENT_LONG_PRESS:
            app_event = APP_MANAGER_KEY_EVENT_LONG_PRESS;
            break;
        default:
            mapped = false;
            break;
        }
    }

    if (mapped)
    {
        const app_manager_input_cb_t callback = s_app_input_callback;
        if (callback != NULL)
        {
            callback(app_key, app_event, s_app_input_context);
        }
    }
}

static esp_err_t _app_runtime_pm_input_register(app_manager_input_cb_t callback,
        void *user_data)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (callback == NULL || s_bsp_input == NULL ||
            s_bsp_input->register_handler == NULL)
    {
        return result;
    }
    if (s_app_input_callback != NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        return result;
    }

    s_app_input_callback = callback;
    s_app_input_context = user_data;
    result = s_bsp_input->register_handler(_app_runtime_pm_bsp_input_callback,
                                           NULL);
    if (result != ESP_OK)
    {
        s_app_input_callback = NULL;
        s_app_input_context = NULL;
    }
    return result;
}

static esp_err_t _app_runtime_pm_input_unregister(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_bsp_input == NULL || s_bsp_input->unregister_handler == NULL)
    {
        return result;
    }
    result = s_bsp_input->unregister_handler();
    if (result == ESP_OK)
    {
        s_app_input_callback = NULL;
        s_app_input_context = NULL;
    }
    return result;
}

static esp_err_t _app_runtime_pm_prepare_failed(
    app_runtime_sleep_context_t *sleep, uint32_t timeout_ms,
    esp_err_t primary_error)
{
    const esp_err_t recovery_result = _app_runtime_pm_complete_sleep(timeout_ms,
                                      sleep);
    if (recovery_result != ESP_OK)
    {
        LOG_E("sleep rollback failed: %s", esp_err_to_name(recovery_result));
    }
    return primary_error;
}

static esp_err_t _app_runtime_pm_prepare_sleep(uint32_t timeout_ms,
        void *context)
{
    app_runtime_sleep_context_t *sleep = context;
    esp_err_t result = ESP_OK;
    if (sleep->wifi_resume_required || sleep->power_resume_required ||
            sleep->input_resume_required)
    {
        result = _app_runtime_pm_complete_sleep(timeout_ms, sleep);
        if (result != ESP_OK)
        {
            return result;
        }
    }

    if (sleep->wifi_participant)
    {
        sleep->wifi_resume_required = true;
        result = wifi_service_suspend(timeout_ms);
        if (result != ESP_OK)
        {
            goto rollback;
        }
    }

    sleep->power_resume_required = true;
    result = power_service_suspend(timeout_ms);
    if (result != ESP_OK)
    {
        goto rollback;
    }

    sleep->input_resume_required = true;
    result = sleep->input->prepare_sleep(timeout_ms);
    if (result != ESP_OK)
    {
        goto rollback;
    }
    if (!app_manager_pm_standby_prepare_guard())
    {
        result = ESP_ERR_INVALID_STATE;
        goto rollback;
    }
    return result;

rollback:
    return _app_runtime_pm_prepare_failed(sleep, timeout_ms, result);
}

static esp_err_t _app_runtime_pm_complete_sleep(uint32_t timeout_ms,
        void *context)
{
    app_runtime_sleep_context_t *sleep = context;
    esp_err_t first_error = ESP_OK;

    if (sleep->input_resume_required)
    {
        const esp_err_t result = sleep->input->complete_sleep(timeout_ms);
        _app_runtime_pm_record_first_error(&first_error, result);
        if (result == ESP_OK)
        {
            sleep->input_resume_required = false;
        }
    }
    if (sleep->power_resume_required)
    {
        const esp_err_t result = power_service_resume(timeout_ms);
        _app_runtime_pm_record_first_error(&first_error, result);
        if (result == ESP_OK)
        {
            sleep->power_resume_required = false;
        }
    }
    if (sleep->wifi_resume_required)
    {
        const esp_err_t result = wifi_service_resume(timeout_ms);
        _app_runtime_pm_record_first_error(&first_error, result);
        if (result == ESP_OK)
        {
            sleep->wifi_resume_required = false;
        }
    }
    return first_error;
}

static void _app_runtime_pm_system_wake_callback(
    const system_pm_wake_event_t *event, void *context)
{
    (void)event;
    (void)context;
    (void)app_manager_pm_notify_system_wake();
}

void app_runtime_pm_reset(void)
{
    memset(&s_sleep_context, 0, sizeof(s_sleep_context));
    s_bsp_power = NULL;
    s_bsp_input = NULL;
    s_app_input_callback = NULL;
    s_app_input_context = NULL;
}

void app_runtime_pm_close_admission(void)
{
    atomic_store_explicit(&s_standby_admitted, false, memory_order_release);
    while (atomic_load_explicit(&s_standby_request_users,
                                memory_order_acquire) != 0U)
    {
        vTaskDelay(1);
    }
}

void app_runtime_pm_open_admission(void)
{
    atomic_store_explicit(&s_standby_admitted, true, memory_order_release);
}

esp_err_t app_runtime_pm_build_system_config(system_pm_config_t *config)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    s_bsp_input = bsp_hal_get_input();
    if (s_bsp_input == NULL)
    {
        return result;
    }

    bsp_wakeup_descriptor_t descriptor;
    result = bsp_get_wakeup_descriptor(&descriptor);
    if (result != ESP_OK)
    {
        return result;
    }

    memset(config, 0, sizeof(*config));
    for (int gpio = 0; gpio < 64; ++gpio)
    {
        const uint64_t bit = UINT64_C(1) << gpio;
        if ((descriptor.gpio_mask & bit) == 0)
        {
            continue;
        }
        if (config->wake_source_count == SYSTEM_PM_MAX_WAKE_SOURCES)
        {
            result = ESP_ERR_INVALID_SIZE;
            return result;
        }
        system_pm_wake_source_t *source =
            &config->wake_sources[config->wake_source_count++];
        source->gpio_num = gpio;
        source->active_level = (descriptor.active_low_mask & bit) != 0 ?
                               SYSTEM_PM_WAKE_LEVEL_LOW :
                               SYSTEM_PM_WAKE_LEVEL_HIGH;
    }
    if (config->wake_source_count == 0)
    {
        result = ESP_ERR_INVALID_STATE;
        return result;
    }

    s_sleep_context.input = s_bsp_input;
    config->prepare_sleep = _app_runtime_pm_prepare_sleep;
    config->complete_sleep = _app_runtime_pm_complete_sleep;
    config->sleep_hook_context = &s_sleep_context;
    config->prepare_timeout_ms = APP_RUNTIME_SLEEP_TIMEOUT_MS;
    config->wake_callback = _app_runtime_pm_system_wake_callback;
    config->commit_guard = _app_runtime_pm_commit_guard;
    config->commit_callback = _app_runtime_pm_commit_callback;
    config->commit_context = NULL;
    return result;
}

app_manager_input_ops_t app_runtime_pm_get_input_ops(void)
{
    return (app_manager_input_ops_t)
    {
        .register_handler = _app_runtime_pm_input_register,
        .unregister_handler = _app_runtime_pm_input_unregister,
    };
}

app_manager_standby_ops_t app_runtime_pm_get_standby_ops(void)
{
    return (app_manager_standby_ops_t)
    {
        .request_standby = _app_runtime_pm_request_standby,
        .cancel_standby = _app_runtime_pm_cancel_standby,
    };
}

esp_err_t app_runtime_pm_prepare_power(bsp_capabilities_t capabilities)
{
    esp_err_t result = ESP_OK;
    if ((capabilities & BSP_CAPABILITY_POWER) != 0)
    {
        s_bsp_power = bsp_hal_get_power();
        if (s_bsp_power == NULL)
        {
            result = ESP_ERR_INVALID_STATE;
            return result;
        }
        const power_service_power_ops_t power_ops =
        {
            .is_available = s_bsp_power->is_available,
            .get_info = _app_runtime_pm_power_get_info,
        };
        result = power_service_register_power_ops(&power_ops);
    }
    else
    {
        LOG_W("PMU unavailable; power snapshots remain invalid");
    }
    return result;
}

void app_runtime_pm_clear_power(void)
{
    s_bsp_power = NULL;
}

void app_runtime_pm_set_wifi_participant(bool enabled)
{
    s_sleep_context.wifi_participant = enabled;
}

void app_runtime_pm_detach_bsp(void)
{
    s_bsp_input = NULL;
    s_sleep_context.input = NULL;
    s_app_input_callback = NULL;
    s_app_input_context = NULL;
}

#include "apps_integration_runtime.h"

#include "event_bus.h"
#include "host_wifi_service.h"
#include "power_service.h"
#include "time_service.h"

EVENT_BUS_DEFINE_ID(POWER_SERVICE_MSG);

static uint8_t s_brightness = 192;

void host_runtime_reset(void)
{
    s_brightness = 192;
    host_lv_reset();
    host_wifi_service_reset();
}

esp_err_t power_service_get_snapshot(power_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = (power_service_snapshot_t)
    {
        .info =
        {
            .battery_voltage_mv = 3910,
            .battery_percent = 78,
            .is_charging = false,
            .is_vbus_connected = false,
        },
        .sampled_at_ms = 123456,
        .valid = true,
    };
    return ESP_OK;
}

esp_err_t time_service_get_local(struct tm *local_time)
{
    if (local_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *local_time = (struct tm)
    {
        .tm_sec = 0,
        .tm_min = 30,
        .tm_hour = 8,
        .tm_mday = 16,
        .tm_mon = 6,
        .tm_year = 126,
        .tm_wday = 4,
        .tm_yday = 196,
        .tm_isdst = 0,
    };
    return ESP_OK;
}

time_service_quality_t time_service_get_quality(void)
{
    return TIME_SERVICE_QUALITY_RTC;
}

esp_err_t app_manager_screen_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    return ESP_OK;
}

esp_err_t app_manager_screen_set_brightness_temp(uint8_t brightness)
{
    (void)brightness;
    return ESP_OK;
}

uint8_t app_manager_screen_get_brightness(void)
{
    return s_brightness;
}

esp_err_t app_manager_pm_request_screen_off(void)
{
    return ESP_OK;
}

int32_t app_manager_pm_get_timeout_ms(void)
{
    return 30000;
}

const char *esp_err_to_name(esp_err_t error)
{
    switch (error)
    {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FINISHED:
        return "ESP_ERR_NOT_FINISHED";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

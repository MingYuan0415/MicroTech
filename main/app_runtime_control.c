#define DBG_TAG "app_runtime_control"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_runtime_control.h"

#include "connectivity_manager.h"
#include "device_link_service.h"
#include "time_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static char s_clock_time[8];
static char s_clock_date[32];

static esp_err_t _control_get_wifi(app_manager_radio_state_t *state,
                                   const char **ssid)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    connectivity_manager_status_snapshot_t status;
    if (connectivity_manager_get_status(&status) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (status.state == CONNECTIVITY_MANAGER_STATE_OFFLINE)
    {
        *state = APP_MANAGER_RADIO_OFF;
    }
    else if (status.state == CONNECTIVITY_MANAGER_STATE_IP_READY)
    {
        *state = APP_MANAGER_RADIO_CONNECTED;
    }
    else
    {
        *state = APP_MANAGER_RADIO_ON_IDLE;
    }
    if (ssid != NULL)
    {
        *ssid = *state == APP_MANAGER_RADIO_CONNECTED ? status.ssid : NULL;
    }
    return ESP_OK;
}

static esp_err_t _control_set_wifi(bool enabled)
{
    connectivity_manager_operation_id_t operation = 0U;
    if (!enabled)
    {
        return connectivity_manager_request_disconnect(&operation);
    }
    connectivity_manager_status_snapshot_t status;
    if (connectivity_manager_get_status(&status) == ESP_OK &&
            status.saved_profile)
    {
        return connectivity_manager_request_reconnect_saved(&operation);
    }
    return connectivity_manager_request_scan(&operation);
}

static esp_err_t _control_get_bluetooth(app_manager_radio_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    device_link_service_status_t status;
    if (device_link_service_get_status(&status) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!status.enabled)
    {
        *state = APP_MANAGER_RADIO_OFF;
    }
    else if (status.bound)
    {
        *state = APP_MANAGER_RADIO_CONNECTED;
    }
    else
    {
        *state = APP_MANAGER_RADIO_ON_IDLE;
    }
    return ESP_OK;
}

static esp_err_t _control_set_bluetooth(bool enabled)
{
    return device_link_service_set_enabled(enabled, 0U);
}

static esp_err_t _control_get_clock(const char **time_text,
                                    const char **date_text)
{
    static const char *const weekdays[] =
    {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
    };
    struct tm now;
    if (time_service_get_local(&now) != ESP_OK ||
            time_service_get_quality() == TIME_SERVICE_QUALITY_INVALID)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)snprintf(s_clock_time, sizeof(s_clock_time), "%02d:%02d",
                   now.tm_hour, now.tm_min);
    int weekday = now.tm_wday;
    if (weekday < 0 || weekday > 6)
    {
        weekday = 0;
    }
    (void)snprintf(s_clock_date, sizeof(s_clock_date), "%d月%d日 %s",
                   now.tm_mon + 1, now.tm_mday, weekdays[weekday]);
    if (time_text != NULL)
    {
        *time_text = s_clock_time;
    }
    if (date_text != NULL)
    {
        *date_text = s_clock_date;
    }
    return ESP_OK;
}

app_manager_control_center_ops_t app_runtime_control_get_ops(void)
{
    const app_manager_control_center_ops_t ops =
    {
        .get_wifi = _control_get_wifi,
        .set_wifi = _control_set_wifi,
        .get_bluetooth = _control_get_bluetooth,
        .set_bluetooth = _control_set_bluetooth,
        .get_clock = _control_get_clock,
    };
    return ops;
}

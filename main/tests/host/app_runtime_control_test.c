#include "app_runtime_control.h"

#include "connectivity_manager.h"
#include "device_link_service.h"
#include "time_service.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static connectivity_manager_status_snapshot_t s_connectivity;
static esp_err_t s_connectivity_result;
static esp_err_t s_scan_result;
static esp_err_t s_disconnect_result;
static esp_err_t s_reconnect_result;
static unsigned s_scan_calls;
static unsigned s_disconnect_calls;
static unsigned s_reconnect_calls;
static connectivity_manager_operation_id_t s_next_operation = 7U;
static device_link_service_status_t s_device_link;
static esp_err_t s_device_link_result;
static esp_err_t s_set_enabled_result;
static bool s_set_enabled_value;
static unsigned s_set_enabled_calls;
static uint32_t s_set_enabled_timeout;
static esp_err_t s_local_result;
static struct tm s_local_time;
static time_service_quality_t s_quality;

static void _reset(void)
{
    memset(&s_connectivity, 0, sizeof(s_connectivity));
    s_connectivity_result = ESP_OK;
    s_scan_result = ESP_OK;
    s_disconnect_result = ESP_OK;
    s_reconnect_result = ESP_OK;
    s_scan_calls = 0U;
    s_disconnect_calls = 0U;
    s_reconnect_calls = 0U;
    s_next_operation = 7U;
    memset(&s_device_link, 0, sizeof(s_device_link));
    s_device_link_result = ESP_OK;
    s_set_enabled_result = ESP_OK;
    s_set_enabled_value = false;
    s_set_enabled_calls = 0U;
    s_set_enabled_timeout = UINT32_MAX;
    s_local_result = ESP_OK;
    memset(&s_local_time, 0, sizeof(s_local_time));
    s_quality = TIME_SERVICE_QUALITY_RTC;
}

static app_manager_control_center_ops_t _ops(void)
{
    const app_manager_control_center_ops_t ops = app_runtime_control_get_ops();

    /* Every callback is always populated; the surface renders an error as an
       unavailable tile rather than checking for a null pointer. */
    assert(ops.get_wifi != NULL);
    assert(ops.set_wifi != NULL);
    assert(ops.get_bluetooth != NULL);
    assert(ops.set_bluetooth != NULL);
    assert(ops.get_bluetooth_transitioning != NULL);
    assert(ops.get_clock != NULL);
    return ops;
}

static void _test_wifi_state_mapping(void)
{
    const app_manager_control_center_ops_t ops = _ops();
    app_manager_radio_state_t state = APP_MANAGER_RADIO_CONNECTED;
    const char *ssid = "stale";

    _reset();
    s_connectivity.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
    assert(ops.get_wifi(&state, &ssid) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_OFF);
    assert(ssid == NULL);

    s_connectivity.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    memcpy(s_connectivity.ssid, "Home", sizeof("Home"));
    ssid = NULL;
    assert(ops.get_wifi(&state, &ssid) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_CONNECTED);
    assert(ssid != NULL && strcmp(ssid, "Home") == 0);

    s_connectivity.state = CONNECTIVITY_MANAGER_STATE_SCANNING;
    ssid = "stale";
    assert(ops.get_wifi(&state, &ssid) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_ON_IDLE);
    /* Only a connected radio has an SSID to render. */
    assert(ssid == NULL);

    /* The SSID output is optional. */
    assert(ops.get_wifi(&state, NULL) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_ON_IDLE);

    assert(ops.get_wifi(NULL, &ssid) == ESP_ERR_INVALID_ARG);
    s_connectivity_result = ESP_FAIL;
    assert(ops.get_wifi(&state, &ssid) == ESP_ERR_INVALID_STATE);
}

static void _test_wifi_actions(void)
{
    const app_manager_control_center_ops_t ops = _ops();

    _reset();
    assert(ops.set_wifi(false) == ESP_OK);
    assert(s_disconnect_calls == 1U);
    assert(s_scan_calls == 0U);
    assert(s_reconnect_calls == 0U);

    /* Enabling without a saved profile starts discovery. */
    s_connectivity.saved_profile = false;
    assert(ops.set_wifi(true) == ESP_OK);
    assert(s_scan_calls == 1U);
    assert(s_reconnect_calls == 0U);

    /* A saved profile is rejoined instead of rescanning. */
    s_connectivity.saved_profile = true;
    assert(ops.set_wifi(true) == ESP_OK);
    assert(s_reconnect_calls == 1U);
    assert(s_scan_calls == 1U);

    /* A failed status read also falls back to discovery. */
    s_connectivity_result = ESP_FAIL;
    assert(ops.set_wifi(true) == ESP_OK);
    assert(s_scan_calls == 2U);

    /* Request failures propagate unchanged. */
    s_connectivity_result = ESP_OK;
    s_disconnect_result = ESP_ERR_INVALID_STATE;
    assert(ops.set_wifi(false) == ESP_ERR_INVALID_STATE);
    s_connectivity.saved_profile = false;
    s_scan_result = ESP_ERR_NO_MEM;
    assert(ops.set_wifi(true) == ESP_ERR_NO_MEM);
}

static void _test_bluetooth(void)
{
    const app_manager_control_center_ops_t ops = _ops();
    app_manager_radio_state_t state = APP_MANAGER_RADIO_CONNECTED;

    _reset();
    s_device_link.enabled = false;
    assert(ops.get_bluetooth(&state) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_OFF);

    s_device_link.enabled = true;
    assert(ops.get_bluetooth(&state) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_ON_IDLE);

    s_device_link.client_connected = true;
    assert(ops.get_bluetooth(&state) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_CONNECTED);

    assert(ops.get_bluetooth(NULL) == ESP_ERR_INVALID_ARG);
    s_device_link_result = ESP_FAIL;
    assert(ops.get_bluetooth(&state) == ESP_ERR_INVALID_STATE);

    /* The setter forwards the request with a non-blocking timeout. */
    _reset();
    assert(ops.set_bluetooth(true) == ESP_OK);
    assert(s_set_enabled_calls == 1U);
    assert(s_set_enabled_value);
    assert(s_set_enabled_timeout == 0U);
    s_set_enabled_result = ESP_FAIL;
    assert(ops.set_bluetooth(false) == ESP_FAIL);

    /* Transitioning is only reported when the status is readable. */
    _reset();
    s_device_link.transitioning = true;
    assert(ops.get_bluetooth_transitioning());
    s_device_link_result = ESP_FAIL;
    assert(!ops.get_bluetooth_transitioning());
}

static void _test_clock(void)
{
    const app_manager_control_center_ops_t ops = _ops();
    const char *time_text = NULL;
    const char *date_text = NULL;

    _reset();
    s_local_time.tm_hour = 9;
    s_local_time.tm_min = 5;
    s_local_time.tm_mon = 6;
    s_local_time.tm_mday = 19;
    s_local_time.tm_wday = 3;
    assert(ops.get_clock(&time_text, &date_text) == ESP_OK);
    assert(time_text != NULL && strcmp(time_text, "09:05") == 0);
    assert(date_text != NULL && strcmp(date_text, "7月19日 周三") == 0);

    /* An out-of-range weekday is clamped instead of indexing the table. */
    s_local_time.tm_wday = 9;
    assert(ops.get_clock(&time_text, &date_text) == ESP_OK);
    assert(strcmp(date_text, "7月19日 周日") == 0);

    /* Both outputs are optional. */
    assert(ops.get_clock(NULL, NULL) == ESP_OK);

    /* An unsynchronized clock is unavailable, not midnight. */
    s_quality = TIME_SERVICE_QUALITY_INVALID;
    assert(ops.get_clock(&time_text, &date_text) == ESP_ERR_INVALID_STATE);
    s_quality = TIME_SERVICE_QUALITY_RTC;
    s_local_result = ESP_FAIL;
    assert(ops.get_clock(&time_text, &date_text) == ESP_ERR_INVALID_STATE);
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    if (s_connectivity_result != ESP_OK)
    {
        return s_connectivity_result;
    }
    *snapshot = s_connectivity;
    return ESP_OK;
}

esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *out_operation_id)
{
    assert(out_operation_id != NULL);
    ++s_scan_calls;
    *out_operation_id = s_next_operation++;
    return s_scan_result;
}

esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *out_operation_id)
{
    assert(out_operation_id != NULL);
    ++s_disconnect_calls;
    *out_operation_id = s_next_operation++;
    return s_disconnect_result;
}

esp_err_t connectivity_manager_request_reconnect_saved(
    connectivity_manager_operation_id_t *out_operation_id)
{
    assert(out_operation_id != NULL);
    ++s_reconnect_calls;
    *out_operation_id = s_next_operation++;
    return s_reconnect_result;
}

esp_err_t device_link_service_get_status(
    device_link_service_status_t *status)
{
    assert(status != NULL);
    if (s_device_link_result != ESP_OK)
    {
        return s_device_link_result;
    }
    *status = s_device_link;
    return ESP_OK;
}

esp_err_t device_link_service_set_enabled(bool enabled, uint32_t timeout_ms)
{
    ++s_set_enabled_calls;
    s_set_enabled_value = enabled;
    s_set_enabled_timeout = timeout_ms;
    return s_set_enabled_result;
}

esp_err_t time_service_get_local(struct tm *local_time)
{
    assert(local_time != NULL);
    if (s_local_result != ESP_OK)
    {
        return s_local_result;
    }
    *local_time = s_local_time;
    return ESP_OK;
}

time_service_quality_t time_service_get_quality(void)
{
    return s_quality;
}

int main(void)
{
    _test_wifi_state_mapping();
    _test_wifi_actions();
    _test_bluetooth();
    _test_clock();
    printf("app_runtime_control host tests passed\n");
    return 0;
}

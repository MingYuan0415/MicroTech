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
static uint8_t s_volume;
static esp_err_t s_volume_get_result;
static esp_err_t s_volume_set_result;
static uint8_t s_volume_set_value;
static unsigned s_volume_set_calls;
/* Persisted-volume service fake. */
static esp_err_t s_volume_persisted_result;
static uint8_t s_volume_persisted_value;
static unsigned s_volume_persisted_calls;

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
    s_volume = 60U;
    s_volume_get_result = ESP_OK;
    s_volume_set_result = ESP_OK;
    s_volume_set_value = UINT8_MAX;
    s_volume_set_calls = 0U;
    s_volume_persisted_result = ESP_OK;
    s_volume_persisted_value = UINT8_MAX;
    s_volume_persisted_calls = 0U;
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
    assert(ops.get_volume != NULL);
    assert(ops.set_volume != NULL);
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
    s_connectivity.wifi_enabled = true;
    memcpy(s_connectivity.ssid, "Home", sizeof("Home"));
    ssid = NULL;
    assert(ops.get_wifi(&state, &ssid) == ESP_OK);
    assert(state == APP_MANAGER_RADIO_CONNECTED);
    assert(ssid != NULL && strcmp(ssid, "Home") == 0);

    s_connectivity.state = CONNECTIVITY_MANAGER_STATE_SCANNING;
    s_connectivity.wifi_enabled = true;
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

    /* Enabling always starts the manager-owned connection cycle. */
    assert(ops.set_wifi(true) == ESP_OK);
    assert(s_reconnect_calls == 1U);
    assert(s_scan_calls == 0U);

    s_connectivity.saved_profile = true;
    assert(ops.set_wifi(true) == ESP_OK);
    assert(s_reconnect_calls == 2U);
    assert(s_scan_calls == 0U);

    /* A failed status read no longer changes the enable API. */
    s_connectivity_result = ESP_FAIL;
    assert(ops.set_wifi(true) == ESP_OK);
    assert(s_reconnect_calls == 3U);

    /* Request failures propagate unchanged. */
    s_connectivity_result = ESP_OK;
    s_disconnect_result = ESP_ERR_INVALID_STATE;
    assert(ops.set_wifi(false) == ESP_ERR_INVALID_STATE);
    s_reconnect_result = ESP_ERR_NO_MEM;
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

esp_err_t connectivity_manager_request_wifi_disable(
    connectivity_manager_operation_id_t *out_operation_id)
{
    assert(out_operation_id != NULL);
    ++s_disconnect_calls;
    *out_operation_id = s_next_operation++;
    return s_disconnect_result;
}

esp_err_t connectivity_manager_request_wifi_enable(
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

static void _test_volume(void)
{
    const app_manager_control_center_ops_t ops = _ops();
    uint8_t percent = 0U;

    _reset();
    assert(ops.get_volume(&percent) == ESP_OK);
    assert(percent == 60U);

    /* The surface passes a slider value straight through; out-of-range input
     * is rejected instead of being clamped silently. */
    assert(ops.set_volume(0U) == ESP_OK);
    assert(s_volume_set_calls == 1U);
    assert(s_volume_set_value == 0U);
    assert(s_volume == 0U);
    assert(ops.set_volume(100U) == ESP_OK);
    assert(s_volume_set_value == 100U);
    assert(ops.set_volume(101U) == ESP_ERR_INVALID_ARG);
    assert(s_volume_set_calls == 2U);
    assert(ops.get_volume(NULL) == ESP_ERR_INVALID_ARG);

    /* A refused request reports the service error and leaves the level. */
    s_volume_set_result = ESP_FAIL;
    assert(ops.set_volume(30U) == ESP_FAIL);
    assert(s_volume == 100U);
    percent = 0U;
    assert(ops.get_volume(&percent) == ESP_OK);
    assert(percent == 100U);

    /* An uninitialized service keeps the row disabled through the getter. */
    s_volume_get_result = ESP_ERR_INVALID_STATE;
    assert(ops.get_volume(&percent) == ESP_ERR_INVALID_STATE);
}

/* Volume storage belongs to audio_service, so the bridge is only required to
 * forward the two volume intents without duplicating storage logic. */
static void _test_volume_forwarding(void)
{
    const app_manager_control_center_ops_t ops = _ops();
    assert(ops.set_volume_persisted != NULL);

    /* The preview path reaches the service and nothing else. */
    _reset();
    assert(ops.set_volume(70U) == ESP_OK);
    assert(s_volume == 70U);
    assert(s_volume_set_calls == 1U);
    assert(s_volume_persisted_calls == 0U);

    /* The settle path is forwarded verbatim to the persisted setter. */
    _reset();
    assert(ops.set_volume_persisted(40U) == ESP_OK);
    assert(s_volume_persisted_calls == 1U);
    assert(s_volume_persisted_value == 40U);
    assert(s_volume == 40U);

    /* Errors from the service are returned unchanged. */
    _reset();
    s_volume_persisted_result = ESP_FAIL;
    assert(ops.set_volume_persisted(40U) == ESP_FAIL);

    _reset();
    s_volume_persisted_result = ESP_ERR_INVALID_ARG;
    assert(ops.set_volume_persisted(200U) == ESP_ERR_INVALID_ARG);
}

esp_err_t audio_service_get_volume(uint8_t *percent)
{
    if (s_volume_get_result != ESP_OK)
    {
        return s_volume_get_result;
    }
    assert(percent != NULL);
    *percent = s_volume;
    return ESP_OK;
}

esp_err_t audio_service_set_volume(uint8_t percent)
{
    ++s_volume_set_calls;
    s_volume_set_value = percent;
    if (s_volume_set_result == ESP_OK)
    {
        s_volume = percent;
    }
    return s_volume_set_result;
}

esp_err_t audio_service_set_volume_persisted(uint8_t percent)
{
    ++s_volume_persisted_calls;
    s_volume_persisted_value = percent;
    if (s_volume_persisted_result == ESP_OK)
    {
        s_volume = percent;
    }
    return s_volume_persisted_result;
}

int main(void)
{
    _test_wifi_state_mapping();
    _test_wifi_actions();
    _test_bluetooth();
    _test_clock();
    _test_volume();
    _test_volume_forwarding();
    printf("app_runtime_control host tests passed\n");
    return 0;
}

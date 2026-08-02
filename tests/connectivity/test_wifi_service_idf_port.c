#include "host_wifi_idf.h"
#include "wifi_service_port.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static wifi_ap_record_t _test_ap(const char *ssid, int8_t rssi,
                                 uint8_t channel,
                                 wifi_auth_mode_t authmode)
{
    wifi_ap_record_t record =
    {
        .rssi = rssi,
        .primary = channel,
        .authmode = authmode,
    };
    (void)snprintf((char *)record.ssid, sizeof(record.ssid), "%s", ssid);
    return record;
}

static void _test_scan_normalization(void)
{
    const wifi_ap_record_t raw[] =
    {
        _test_ap("", -5, 1, WIFI_AUTH_OPEN),
        _test_ap("zeta", -65, 1, WIFI_AUTH_WPA2_PSK),
        _test_ap("alpha", -45, 6, WIFI_AUTH_WPA2_PSK),
        _test_ap("alpha", -30, 11, WIFI_AUTH_WPA3_PSK),
        _test_ap("open", -40, 3, WIFI_AUTH_OPEN),
        _test_ap("alpha", -35, 7, WIFI_AUTH_OPEN),
        _test_ap("beta", -55, 8, WIFI_AUTH_ENTERPRISE),
        _test_ap("gamma", -60, 9, WIFI_AUTH_WPA2_PSK),
        _test_ap("delta", -50, 10, WIFI_AUTH_WPA2_PSK),
    };
    host_wifi_idf_set_scan_records(raw, sizeof(raw) / sizeof(raw[0]));
    assert(wifi_service_port_scan_start() == ESP_OK);

    wifi_service_port_scan_record_t records[WIFI_SERVICE_MAX_SCAN_RECORDS];
    size_t count = 0U;
    bool truncated = false;
    const unsigned clears_before = host_wifi_idf_scan_clear_count();
    assert(wifi_service_port_scan_finish(
               records, WIFI_SERVICE_MAX_SCAN_RECORDS,
               &count, &truncated) == ESP_OK);
    assert(count == WIFI_SERVICE_MAX_SCAN_RECORDS);
    assert(truncated);
    assert(host_wifi_idf_scan_clear_count() == clears_before + 1U);
    assert(records[0].rssi == -30);
    assert(records[0].channel == 11U);
    assert(records[0].ssid_length == strlen("alpha"));
    assert(memcmp(records[0].ssid, "alpha", strlen("alpha")) == 0);
    assert(records[1].security == WIFI_SERVICE_SECURITY_OPEN);
    assert(memcmp(records[1].ssid, "alpha", strlen("alpha")) == 0);
    assert(memcmp(records[2].ssid, "open", strlen("open")) == 0);
    assert(memcmp(records[3].ssid, "delta", strlen("delta")) == 0);
    assert(memcmp(records[4].ssid, "beta", strlen("beta")) == 0);
    assert(!wifi_service_port_scan_is_owned());
}

int main(void)
{
    host_wifi_idf_reset();
    assert(CONFIG_ESP_WIFI_NVS_ENABLED == 1);
    assert(wifi_service_port_is_clean());
    assert(wifi_service_port_init() == ESP_OK);

    assert(host_wifi_idf_init_nvs_enabled());
    assert(host_wifi_idf_init_sequence() != 0U);
    assert(host_wifi_idf_storage_sequence() ==
           host_wifi_idf_init_sequence() + 1U);
    assert(host_wifi_idf_storage() == WIFI_STORAGE_RAM);
    assert(host_wifi_idf_mode_sequence() ==
           host_wifi_idf_storage_sequence() + 1U);

    wifi_service_port_event_t event;
    assert(host_wifi_idf_emit_disconnect(
               WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, &event));
    assert(event.type == WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED);
    assert(event.disconnect_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
    assert(event.failure == WIFI_SERVICE_FAILURE_AUTHENTICATION);

    _test_scan_normalization();

    assert(wifi_service_port_deinit() == ESP_OK);
    assert(wifi_service_port_is_clean());
    puts("wifi_service ESP-IDF port configuration regression passed");
    return 0;
}

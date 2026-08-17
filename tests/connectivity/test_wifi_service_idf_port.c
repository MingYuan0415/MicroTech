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

static void _test_credentials_policy_and_ascii_psk(void)
{
    char ssid32[WIFI_SERVICE_SSID_MAX_BYTES];
    char ssid33[WIFI_SERVICE_SSID_MAX_BYTES + 1U];
    char passphrase63[63];
    char psk64[WIFI_SERVICE_PASSWORD_MAX_BYTES];
    char invalid_psk64[WIFI_SERVICE_PASSWORD_MAX_BYTES];
    char password65[WIFI_SERVICE_PASSWORD_MAX_BYTES + 1U];

    memset(ssid32, 's', sizeof(ssid32));
    memset(ssid33, 's', sizeof(ssid33));
    memset(passphrase63, 'p', sizeof(passphrase63));
    memset(psk64, 'a', sizeof(psk64));
    memset(invalid_psk64, 'g', sizeof(invalid_psk64));
    memset(password65, 'a', sizeof(password65));

    wifi_service_credentials_t credentials =
    {
        .ssid = "s",
        .ssid_length = 1U,
        .password = NULL,
        .password_length = 0U,
        .security = WIFI_SERVICE_SECURITY_OPEN,
    };

    assert(wifi_service_credentials_valid(&credentials));
    credentials.ssid = ssid32;
    credentials.ssid_length = sizeof(ssid32);
    assert(wifi_service_credentials_valid(&credentials));
    credentials.ssid = ssid33;
    credentials.ssid_length = sizeof(ssid33);
    assert(!wifi_service_credentials_valid(&credentials));

    static const char nul_ssid[] = {'s', '\0'};
    credentials.ssid = nul_ssid;
    credentials.ssid_length = sizeof(nul_ssid);
    assert(!wifi_service_credentials_valid(&credentials));

    credentials.ssid = "ssid";
    credentials.ssid_length = 4U;
    credentials.security = WIFI_SERVICE_SECURITY_PERSONAL;
    credentials.password = "1234567";
    credentials.password_length = 7U;
    assert(!wifi_service_credentials_valid(&credentials));
    credentials.password = "12345678";
    credentials.password_length = 8U;
    assert(wifi_service_credentials_valid(&credentials));
    credentials.password = passphrase63;
    credentials.password_length = sizeof(passphrase63);
    assert(wifi_service_credentials_valid(&credentials));
    credentials.password = psk64;
    credentials.password_length = sizeof(psk64);
    assert(wifi_service_credentials_valid(&credentials));
    credentials.password = invalid_psk64;
    assert(!wifi_service_credentials_valid(&credentials));
    credentials.password = password65;
    credentials.password_length = sizeof(password65);
    assert(!wifi_service_credentials_valid(&credentials));

    static const char nul_password[] =
    {'1', '2', '3', '4', '\0', '6', '7', '8'};
    credentials.password = nul_password;
    credentials.password_length = sizeof(nul_password);
    assert(!wifi_service_credentials_valid(&credentials));

    wifi_service_port_credentials_t port_credentials;

    memset(&port_credentials, 0, sizeof(port_credentials));
    memcpy(port_credentials.ssid, "ssid", 4U);
    port_credentials.ssid_length = 4U;
    memcpy(port_credentials.password, psk64, sizeof(psk64));
    port_credentials.password_length = sizeof(psk64);
    port_credentials.security = WIFI_SERVICE_SECURITY_PERSONAL;
    assert(wifi_service_port_set_credentials(&port_credentials) == ESP_OK);
    wifi_config_t config;

    assert(host_wifi_idf_last_config(&config));
    assert(memcmp(config.sta.password, psk64, sizeof(psk64)) == 0);
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
    _test_credentials_policy_and_ascii_psk();

    assert(wifi_service_port_deinit() == ESP_OK);
    assert(wifi_service_port_is_clean());
    puts("wifi_service ESP-IDF port configuration regression passed");
    return 0;
}

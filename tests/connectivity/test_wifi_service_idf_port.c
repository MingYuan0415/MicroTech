#include "host_wifi_idf.h"
#include "wifi_service_port.h"

#include <assert.h>
#include <stdio.h>

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

    assert(wifi_service_port_deinit() == ESP_OK);
    assert(wifi_service_port_is_clean());
    puts("wifi_service ESP-IDF port configuration regression passed");
    return 0;
}

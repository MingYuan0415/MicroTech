/*
 * ESP32-C6 coprocessor entry point for the ESP32-P4 host.
 *
 * ESP-Hosted's CP role owns Wi-Fi and the BT controller; this app only brings
 * up NVS and the default event loop. The hosted CP auto-initializes its
 * features (Wi-Fi and, when enabled, the BT controller) at boot.
 */

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "c6_cp";

void app_main(void)
{
    ESP_LOGI(TAG, "MicroTech C6 coprocessor starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

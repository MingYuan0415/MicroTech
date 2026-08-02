#ifndef __CONNECTIVITY_HOST_ESP_WIFI_DEFAULT_H__
#define __CONNECTIVITY_HOST_ESP_WIFI_DEFAULT_H__

#include "esp_netif.h"

esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *netif);

#endif /* __CONNECTIVITY_HOST_ESP_WIFI_DEFAULT_H__ */

#ifndef __CONNECTIVITY_HOST_ESP_NETIF_H__
#define __CONNECTIVITY_HOST_ESP_NETIF_H__

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_netif
{
    uint32_t marker;
} esp_netif_t;

typedef struct esp_netif_config
{
    uint32_t marker;
} esp_netif_config_t;

typedef struct esp_netif_ip_info
{
    struct
    {
        uint32_t addr;
    } ip;
} esp_netif_ip_info_t;

typedef struct ip_event_got_ip
{
    esp_netif_t *esp_netif;
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;

#define IP_EVENT_STA_GOT_IP  1
#define IP_EVENT_STA_LOST_IP 2

esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
void esp_netif_destroy(esp_netif_t *netif);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);

#endif /* __CONNECTIVITY_HOST_ESP_NETIF_H__ */

#ifndef __CONNECTIVITY_HOST_ESP_WIFI_H__
#define __CONNECTIVITY_HOST_ESP_WIFI_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_WIFI_NOT_INIT    0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_NOT_STOPPED 0x3003
#define ESP_ERR_WIFI_NOT_CONNECT 0x3004

typedef enum
{
    WIFI_STORAGE_FLASH = 0,
    WIFI_STORAGE_RAM,
} wifi_storage_t;

typedef enum
{
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA,
} wifi_mode_t;

typedef enum
{
    WIFI_IF_STA = 0,
} wifi_interface_t;

typedef enum
{
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
    WIFI_AUTH_ENTERPRISE,
} wifi_auth_mode_t;

typedef struct wifi_init_config
{
    bool nvs_enable;
    uint32_t magic;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() \
    ((wifi_init_config_t) \
    { \
        .nvs_enable = CONFIG_ESP_WIFI_NVS_ENABLED != 0, \
        .magic = UINT32_C(0x1df1df), \
    })

typedef struct wifi_scan_config
{
    bool show_hidden;
    int scan_type;
} wifi_scan_config_t;

typedef struct wifi_ap_record
{
    uint8_t ssid[33];
    int8_t rssi;
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;

typedef struct wifi_sta_config
{
    uint8_t ssid[32];
    uint8_t password[64];
    int scan_method;
    int sort_method;
    struct
    {
        wifi_auth_mode_t authmode;
    } threshold;
    struct
    {
        bool capable;
        bool required;
    } pmf_cfg;
} wifi_sta_config_t;

typedef struct wifi_config
{
    wifi_sta_config_t sta;
} wifi_config_t;

typedef struct wifi_event_sta_scan_done
{
    uint32_t status;
    uint8_t scan_id;
} wifi_event_sta_scan_done_t;

typedef struct wifi_event_sta_disconnected
{
    uint16_t reason;
} wifi_event_sta_disconnected_t;

#define WIFI_SCAN_TYPE_ACTIVE    1
#define WIFI_ALL_CHANNEL_SCAN    1
#define WIFI_CONNECT_AP_BY_SIGNAL 1

#define WIFI_EVENT_SCAN_DONE        1
#define WIFI_EVENT_STA_CONNECTED    2
#define WIFI_EVENT_STA_DISCONNECTED 3

#define WIFI_REASON_AUTH_EXPIRE                         2
#define WIFI_REASON_ASSOC_NOT_AUTHED                    9
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT              15
#define WIFI_REASON_AUTH_FAIL                           202
#define WIFI_REASON_HANDSHAKE_TIMEOUT                   204
#define WIFI_REASON_NO_AP_FOUND                         201
#define WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY   210
#define WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD   211
#define WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD       212

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number,
                                       wifi_ap_record_t *records);
esp_err_t esp_wifi_set_config(wifi_interface_t interface,
                              const wifi_config_t *config);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);

#endif /* __CONNECTIVITY_HOST_ESP_WIFI_H__ */

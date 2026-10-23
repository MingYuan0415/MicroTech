#ifndef __CONNECTIVITY_HOST_WIFI_IDF_H__
#define __CONNECTIVITY_HOST_WIFI_IDF_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi.h"
#include "wifi_service_port.h"

/** @brief Reset call-order observations in the ESP-IDF Wi-Fi fake. */
void host_wifi_idf_reset(void);
/** @brief Return whether esp_wifi_init received nvs_enable=true. */
bool host_wifi_idf_init_nvs_enabled(void);
/** @brief Return the sequence number of esp_wifi_init. */
unsigned host_wifi_idf_init_sequence(void);
/** @brief Return the sequence number of esp_wifi_set_storage. */
unsigned host_wifi_idf_storage_sequence(void);
/** @brief Return the storage selected by the port. */
wifi_storage_t host_wifi_idf_storage(void);
/** @brief Return the sequence number of esp_wifi_set_mode. */
unsigned host_wifi_idf_mode_sequence(void);
/** @brief Emit one STA disconnect and copy the event submitted by the port. */
bool host_wifi_idf_emit_disconnect(uint16_t reason,
                                   wifi_service_port_event_t *event);
/** @brief Configure the ordered raw AP list returned by the Wi-Fi fake. */
void host_wifi_idf_set_scan_records(const wifi_ap_record_t *records,
                                    size_t count);
/** @brief Return the number of AP-list clear calls. */
unsigned host_wifi_idf_scan_clear_count(void);
/** @brief Copy the most recent station configuration. */
bool host_wifi_idf_last_config(wifi_config_t *config);
/** @brief Fail the next station configuration call with the supplied result. */
void host_wifi_idf_fail_next_set_config(esp_err_t result);
/** @brief Return whether the fake Wi-Fi driver is initialized. */
bool host_wifi_idf_is_initialized(void);
/** @brief Return whether a connect request is still in progress. */
bool host_wifi_idf_is_connecting(void);
/** @brief Return the sequence of the most recent stop call. */
unsigned host_wifi_idf_stop_sequence(void);
/** @brief Return the sequence of the most recent station configuration call. */
unsigned host_wifi_idf_config_sequence(void);

#endif /* __CONNECTIVITY_HOST_WIFI_IDF_H__ */

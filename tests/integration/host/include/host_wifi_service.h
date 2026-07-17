#ifndef __CROSS_LAYER_HOST_WIFI_SERVICE_H__
#define __CROSS_LAYER_HOST_WIFI_SERVICE_H__

#include <stddef.h>

#include "wifi_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wi-Fi fake calls observable by the cross-layer tests. */
typedef enum
{
    HOST_WIFI_SERVICE_CALL_IS_AVAILABLE = 0,
    HOST_WIFI_SERVICE_CALL_SESSION_OPEN,
    HOST_WIFI_SERVICE_CALL_SESSION_CLOSE,
    HOST_WIFI_SERVICE_CALL_REQUEST_SCAN,
    HOST_WIFI_SERVICE_CALL_REQUEST_CONNECT,
    HOST_WIFI_SERVICE_CALL_REQUEST_DISCONNECT,
    HOST_WIFI_SERVICE_CALL_CANCEL,
    HOST_WIFI_SERVICE_CALL_GET_STATUS,
    HOST_WIFI_SERVICE_CALL_GET_SCAN_SNAPSHOT,
    HOST_WIFI_SERVICE_CALL_SECURE_ZERO,
    HOST_WIFI_SERVICE_CALL_COUNT,
} host_wifi_service_call_t;

/** @brief Reset the process-lifetime Wi-Fi fake. */
void host_wifi_service_reset(void);
/**
 * @brief Return how often one service entry point was called.
 * @param call identifies the entry point.
 * @return Current call count, or zero for an invalid identifier.
 */
unsigned host_wifi_service_call_count(host_wifi_service_call_t call);

/** @brief Return the fake's current session identifier. */
wifi_service_session_id_t host_wifi_service_current_session(void);
/** @brief Return the fake's current operation identifier. */
wifi_service_operation_id_t host_wifi_service_current_operation(void);

/**
 * @brief Cache and publish a canonical status snapshot.
 * @param snapshot is the validated status to publish.
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 */
esp_err_t host_wifi_service_publish_status(
    const wifi_service_status_snapshot_t *snapshot);
/**
 * @brief Cache and publish a canonical scan snapshot.
 * @param snapshot is the validated scan to publish.
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 */
esp_err_t host_wifi_service_publish_scan(
    const wifi_service_scan_snapshot_t *snapshot);
/**
 * @brief Publish a scan payload without updating the canonical cache.
 * @param payload points to the payload bytes.
 * @param payload_size is the payload size in bytes.
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 */
esp_err_t host_wifi_service_publish_raw_scan(const void *payload,
        size_t payload_size);

#ifdef __cplusplus
}
#endif

#endif /* __CROSS_LAYER_HOST_WIFI_SERVICE_H__ */

#ifndef __CROSS_LAYER_HOST_CONNECTIVITY_MANAGER_H__
#define __CROSS_LAYER_HOST_CONNECTIVITY_MANAGER_H__

#include <stddef.h>

#include "connectivity_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Connectivity-manager calls observable by cross-layer tests. */
typedef enum
{
    HOST_CONNECTIVITY_MANAGER_CALL_IS_AVAILABLE = 0,
    HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_SCAN,
    HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_CONNECT,
    HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_DISCONNECT,
    HOST_CONNECTIVITY_MANAGER_CALL_RECONNECT_SAVED,
    HOST_CONNECTIVITY_MANAGER_CALL_FORGET,
    HOST_CONNECTIVITY_MANAGER_CALL_SET_AUTO_CONNECT,
    HOST_CONNECTIVITY_MANAGER_CALL_CANCEL,
    HOST_CONNECTIVITY_MANAGER_CALL_GET_STATUS,
    HOST_CONNECTIVITY_MANAGER_CALL_GET_SCAN_SNAPSHOT,
    HOST_CONNECTIVITY_MANAGER_CALL_COUNT,
} host_connectivity_manager_call_t;

/** @brief Reset the process-lifetime connectivity-manager fake. */
void host_connectivity_manager_reset(void);

/** @brief Return how often one manager entry point was called. */
unsigned host_connectivity_manager_call_count(
    host_connectivity_manager_call_t call);

/** @brief Return the fake's current foreground operation identifier. */
connectivity_manager_operation_id_t
host_connectivity_manager_current_operation(void);

/** @brief Replace cached status without publishing an event. */
esp_err_t host_connectivity_manager_cache_status(
    const connectivity_manager_status_snapshot_t *snapshot);

/** @brief Cache and publish a canonical status snapshot. */
esp_err_t host_connectivity_manager_publish_status(
    const connectivity_manager_status_snapshot_t *snapshot);

/** @brief Cache and publish a canonical scan snapshot. */
esp_err_t host_connectivity_manager_publish_scan(
    const connectivity_manager_scan_snapshot_t *snapshot);

/** @brief Publish a scan payload without changing the canonical cache. */
esp_err_t host_connectivity_manager_publish_raw_scan(const void *payload,
        size_t payload_size);

#ifdef __cplusplus
}
#endif

#endif /* __CROSS_LAYER_HOST_CONNECTIVITY_MANAGER_H__ */

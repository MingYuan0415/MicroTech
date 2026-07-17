#ifndef __NETWORK_RUNTIME_H__
#define __NETWORK_RUNTIME_H__

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process-lifetime ownership state for one network foundation.
 */
typedef enum
{
    NETWORK_RUNTIME_RESOURCE_UNINITIALIZED = 0,
    NETWORK_RUNTIME_RESOURCE_OWNED,
    NETWORK_RUNTIME_RESOURCE_SHARED,
} network_runtime_resource_state_t;

/**
 * @brief Snapshot of the process-wide ESP-NETIF and event-loop state.
 */
typedef struct network_runtime_status
{
    network_runtime_resource_state_t netif;              /**< ESP-NETIF state. */
    network_runtime_resource_state_t default_event_loop; /**< Event-loop state. */
    bool ready;                                          /**< Both resources ready. */
} network_runtime_status_t;

/**
 * @brief Initialize the process-wide ESP-NETIF and default event loop.
 *
 * @note A successful step is retained because ESP-IDF does not support
 *       esp_netif_deinit(). A later call retries only a missing step.
 *
 * @return ESP_OK when both resources are ready; ESP_ERR_INVALID_STATE when a
 *         concurrent initialization is active; otherwise an ESP-IDF error.
 *
 * @warning This is a potentially blocking task-context operation.
 */
esp_err_t network_runtime_init(void);

/**
 * @brief Read a thread-safe, non-blocking network foundation snapshot.
 *
 * @return Current resource states and aggregate readiness.
 */
network_runtime_status_t network_runtime_get_status(void);

/**
 * @brief Report whether both network foundations are ready.
 *
 * @return true when both foundations are ready; false otherwise.
 */
bool network_runtime_is_ready(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __NETWORK_RUNTIME_H__ */

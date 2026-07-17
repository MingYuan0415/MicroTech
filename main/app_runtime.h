#ifndef __APP_RUNTIME_H__
#define __APP_RUNTIME_H__

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the application runtime and all owned services.
 *
 * @return ESP_OK when the runtime is ready, otherwise an ESP-IDF error.
 *
 * @warning Call from task context. The caller must serialize start and stop.
 */
esp_err_t app_runtime_start(void);

/**
 * @brief Stop owned services in reverse initialization order.
 *
 * @note Unresolved ownership is retained for a later retry. Process-lifetime
 *       logging, event-bus, and network foundations remain initialized.
 *
 * @return ESP_OK when all owned services stop, otherwise an ESP-IDF error.
 *
 * @warning This is a blocking task-context operation.
 */
esp_err_t app_runtime_stop(void);

/**
 * @brief Report whether the application runtime is ready.
 *
 * @return true when the runtime is ready; false otherwise.
 */
bool app_runtime_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_RUNTIME_H__ */

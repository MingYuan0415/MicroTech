#ifndef __P4_RADIO_RUNTIME_H__
#define __P4_RADIO_RUNTIME_H__

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the ESP-Hosted transport for the on-board ESP32-C6.
 *
 * On the ESP32-P4 the C6 owns Wi-Fi and the BT controller. ESP-Hosted is
 * configured with CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n, so the
 * transport core and the SDIO link must be initialized explicitly before
 * ble_runtime or wifi_service starts. This function is idempotent.
 *
 * @return ESP_OK when the hosted core and transport are ready; otherwise the
 *         first initialization error.
 */
esp_err_t p4_radio_runtime_init(void);

/**
 * @brief Tear down the ESP-Hosted transport after all radio users stopped.
 *
 * @return ESP_OK when already stopped or released successfully; otherwise the
 *         ESP-Hosted teardown error.
 */
esp_err_t p4_radio_runtime_deinit(void);

/** @brief Report whether the hosted core and SDIO link are ready. */
bool p4_radio_runtime_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __P4_RADIO_RUNTIME_H__ */

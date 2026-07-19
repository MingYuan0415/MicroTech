#ifndef __APP_RUNTIME_PM_H__
#define __APP_RUNTIME_PM_H__

#include "app_manager_types.h"
#include "bsp_hal.h"
#include "esp_err.h"
#include "system_pm.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset runtime PM bridges after standby admission has closed.
 */
void app_runtime_pm_reset(void);

/**
 * @brief Close standby admission and wait for admitted requests to leave.
 */
void app_runtime_pm_close_admission(void);

/**
 * @brief Open standby admission after runtime startup commits.
 */
void app_runtime_pm_open_admission(void);

/**
 * @brief Build the system PM configuration from registered BSP operations.
 *
 * @param config receives the complete system PM configuration.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t app_runtime_pm_build_system_config(system_pm_config_t *config);

/**
 * @brief Return the input bridge used by the application manager.
 *
 * @return Application-manager input operations.
 */
app_manager_input_ops_t app_runtime_pm_get_input_ops(void);

/**
 * @brief Return the standby bridge used by the application manager.
 *
 * @return Application-manager standby operations.
 */
app_manager_standby_ops_t app_runtime_pm_get_standby_ops(void);

/**
 * @brief Register available BSP power operations with the power service.
 *
 * @param capabilities identifies the initialized BSP capabilities.
 *
 * @return ESP_OK when registration is complete, otherwise an ESP-IDF error.
 */
esp_err_t app_runtime_pm_prepare_power(bsp_capabilities_t capabilities);

/**
 * @brief Clear the power bridge after power-service shutdown.
 */
void app_runtime_pm_clear_power(void);

/**
 * @brief Set whether Wi-Fi participates in sleep transactions.
 *
 * @param enabled is true while the runtime owns Wi-Fi service resources.
 */
void app_runtime_pm_set_wifi_participant(bool enabled);

/** @brief Set whether the IMU worker participates in sleep transactions. */
void app_runtime_pm_set_imu_participant(bool enabled);

/** @brief Set whether audio streaming participates in sleep transactions. */
void app_runtime_pm_set_audio_participant(bool enabled);

/** @brief Set whether RTC polling participates in sleep transactions. */
void app_runtime_pm_set_time_participant(bool enabled);

/**
 * @brief Detach BSP-backed input state after BSP shutdown.
 */
void app_runtime_pm_detach_bsp(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_RUNTIME_PM_H__ */

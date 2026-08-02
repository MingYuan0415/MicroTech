#ifndef __APP_PRODUCT_CONFIG_H__
#define __APP_PRODUCT_CONFIG_H__

#include <stdint.h>

#include "audio_service.h"
#include "connectivity_manager.h"
#include "imu_service.h"
#include "power_service.h"
#include "sd_storage_service.h"
#include "time_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Runtime product policy owned by the root application. */
typedef struct app_product_config
{
    audio_service_init_config_t audio; /**< Audio format and initial output state. */
    sd_storage_service_config_t sd;    /**< Normal removable-storage mount policy. */
    imu_service_config_t imu;          /**< IMU sampling and scheduling policy. */
    power_service_config_t power;      /**< PMU sampling and scheduling policy. */
    time_service_config_t time;        /**< Timezone, SNTP, and worker policy. */
    connectivity_manager_config_t connectivity; /**< Wi-Fi policy workers. */
    uint32_t system_pm_task_priority;  /**< System standby worker priority. */
    uint32_t app_control_task_priority; /**< App-control worker priority. */
} app_product_config_t;

/** @brief Return the immutable product policy for this firmware image. */
const app_product_config_t *app_product_config_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_PRODUCT_CONFIG_H__ */

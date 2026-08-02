#ifndef __HOST_PROVISIONING_SERVICE_H__
#define __HOST_PROVISIONING_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "provisioning_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the process-lifetime provisioning-service fake. */
void host_provisioning_service_reset(void);

/** @brief Return how often the provisioning window was opened. */
unsigned host_provisioning_service_open_count(void);

/** @brief Return how often the provisioning window was closed. */
unsigned host_provisioning_service_close_count(void);

/** @brief Cache and publish one provisioning status snapshot. */
esp_err_t host_provisioning_service_publish_status(
    const provisioning_service_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_PROVISIONING_SERVICE_H__ */

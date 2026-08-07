#ifndef __HOST_DEVICE_LINK_SERVICE_H__
#define __HOST_DEVICE_LINK_SERVICE_H__

#include "device_link_service.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the process-lifetime Device Link service fake. */
void host_device_link_service_reset(void);

/** @brief Return how often the binding window was opened. */
unsigned host_device_link_service_open_count(void);

/** @brief Return how often the binding window was closed. */
unsigned host_device_link_service_close_count(void);

/** @brief Cache and publish one Device Link status snapshot. */
esp_err_t host_device_link_service_publish_status(
    const device_link_service_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DEVICE_LINK_SERVICE_H__ */

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

/** @brief Configure binding-confirmation command admission. */
void host_device_link_service_set_confirm_result(esp_err_t result);

/** @brief Return how often a binding-confirmation command was submitted. */
unsigned host_device_link_service_confirm_count(void);

/** @brief Return the most recently submitted confirmation token. */
device_link_confirmation_token_t
host_device_link_service_last_confirmation_token(void);

/** @brief Cache and publish one Device Link status snapshot. */
esp_err_t host_device_link_service_publish_status(
    const device_link_service_status_t *status);

/** @brief Offer one Numeric Comparison through the production reducer. */
esp_err_t host_device_link_service_offer_numeric_comparison(
    device_link_confirmation_token_t token, uint32_t numeric_comparison);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DEVICE_LINK_SERVICE_H__ */

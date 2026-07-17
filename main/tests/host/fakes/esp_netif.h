#ifndef __ESP_NETIF_H__
#define __ESP_NETIF_H__

#include "esp_err.h"

/**
 * @brief Initialize the fake ESP-NETIF foundation.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_netif_init(void);

#endif /* __ESP_NETIF_H__ */

#ifndef __ESP_EVENT_H__
#define __ESP_EVENT_H__

#include "esp_err.h"

/**
 * @brief Create the fake default event loop.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_event_loop_create_default(void);

#endif /* __ESP_EVENT_H__ */

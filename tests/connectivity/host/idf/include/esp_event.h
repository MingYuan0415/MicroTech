#ifndef __CONNECTIVITY_HOST_ESP_EVENT_H__
#define __CONNECTIVITY_HOST_ESP_EVENT_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *argument,
                                    esp_event_base_t event_base,
                                    int32_t event_id, void *event_data);

extern esp_event_base_t WIFI_EVENT;
extern esp_event_base_t IP_EVENT;

#define ESP_EVENT_ANY_ID (-1)
#define ESP_EVENT_DEFINE_BASE(identifier) \
    esp_event_base_t identifier = #identifier

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t handler, void *argument,
    esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_instance_t instance);
esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id,
                         const void *event_data, size_t event_data_size,
                         uint32_t ticks_to_wait);

#endif /* __CONNECTIVITY_HOST_ESP_EVENT_H__ */

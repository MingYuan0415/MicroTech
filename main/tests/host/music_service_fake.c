#include "music_service.h"

#include <string.h>

esp_err_t music_service_init(const music_service_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t music_service_deinit(void)
{
    return ESP_OK;
}
esp_err_t music_service_set_network_ready(bool ready)
{
    (void)ready;
    return ESP_OK;
}
esp_err_t music_service_reset_persisted_state(void)
{
    return ESP_OK;
}
bool music_service_is_playing(void)
{
    return false;
}

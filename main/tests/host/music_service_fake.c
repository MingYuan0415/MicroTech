#include "music_service.h"

EVENT_BUS_DEFINE_ID(TIME_SERVICE_MSG);

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

esp_err_t music_service_set_time_ready(bool ready)
{
    (void)ready;
    return ESP_OK;
}

esp_err_t music_service_get_diagnostics(music_service_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
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

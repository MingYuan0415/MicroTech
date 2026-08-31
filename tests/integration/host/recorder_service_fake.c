#include "recorder_service.h"

#include <stddef.h>

static recorder_service_snapshot_t s_snapshot;

esp_err_t recorder_service_init(const recorder_service_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t recorder_service_deinit(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_get_snapshot(recorder_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_snapshot;
    return ESP_ERR_INVALID_STATE;
}

bool recorder_service_is_busy(void)
{
    return false;
}

esp_err_t recorder_service_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t recorder_service_pause(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_resume(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_stop(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_play(const char *name)
{
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t recorder_service_stop_playback(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_delete(const char *name)
{
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

#include "timer_service.h"
#include "recorder_service.h"
#include "onboarding_service.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool recorder_service_is_busy(void)
{
    return false;
}

esp_err_t recorder_service_init(const recorder_service_config_t *config)
{
    return config != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t recorder_service_deinit(void)
{
    return ESP_OK;
}

esp_err_t onboarding_service_init(void)
{
    return ESP_OK;
}

esp_err_t onboarding_service_deinit(void)
{
    return ESP_OK;
}

esp_err_t onboarding_service_reset(void)
{
    return ESP_OK;
}

esp_err_t onboarding_service_get_state(onboarding_service_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *state = ONBOARDING_SERVICE_COMPLETED;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return 0;
}

esp_err_t timer_service_init(const timer_service_config_t *config)
{
    return config != NULL && config->monotonic_time_us != NULL ? ESP_OK :
           ESP_ERR_INVALID_ARG;
}

esp_err_t timer_service_deinit(void)
{
    return ESP_OK;
}

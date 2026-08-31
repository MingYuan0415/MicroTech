#include "onboarding_service.h"

#include <stdbool.h>
#include <stddef.h>

static bool s_initialized;
static onboarding_service_state_t s_state;

esp_err_t onboarding_service_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_initialized = true;
    s_state = ONBOARDING_SERVICE_PENDING;
    return ESP_OK;
}

esp_err_t onboarding_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_initialized = false;
    return ESP_OK;
}

esp_err_t onboarding_service_get_state(onboarding_service_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *state = s_state;
    return ESP_OK;
}

esp_err_t onboarding_service_defer(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = ONBOARDING_SERVICE_DEFERRED;
    return ESP_OK;
}

esp_err_t onboarding_service_complete(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = ONBOARDING_SERVICE_COMPLETED;
    return ESP_OK;
}

esp_err_t onboarding_service_reset(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = ONBOARDING_SERVICE_PENDING;
    return ESP_OK;
}

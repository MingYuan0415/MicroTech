#include "timer_service.h"
#include "file_service.h"
#include "recorder_service.h"
#include "onboarding_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static esp_err_t s_file_service_init_result;
static bool s_file_service_initialized;
static size_t s_file_service_deinit_count;

void host_file_service_reset(void)
{
    s_file_service_init_result = ESP_OK;
    s_file_service_initialized = false;
    s_file_service_deinit_count = 0U;
}

void host_file_service_set_init_result(esp_err_t result)
{
    s_file_service_init_result = result;
}

bool host_file_service_is_initialized(void)
{
    return s_file_service_initialized;
}

size_t host_file_service_deinit_count(void)
{
    return s_file_service_deinit_count;
}

bool g_host_onboarding_pending;

void host_onboarding_set_pending(bool pending)
{
    g_host_onboarding_pending = pending;
}

bool g_host_recorder_busy;

void host_recorder_set_busy(bool busy)
{
    g_host_recorder_busy = busy;
}

bool recorder_service_is_busy(void)
{
    return g_host_recorder_busy;
}

esp_err_t recorder_service_init(const recorder_service_config_t *config)
{
    return config != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t file_service_init(const file_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_file_service_init_result != ESP_OK)
    {
        return s_file_service_init_result;
    }
    s_file_service_initialized = true;
    return ESP_OK;
}

esp_err_t file_service_deinit(void)
{
    ++s_file_service_deinit_count;
    if (!s_file_service_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_file_service_initialized = false;
    return ESP_OK;
}

bool file_service_is_busy(void)
{
    return false;
}

esp_err_t file_service_suspend(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t file_service_resume(void)
{
    return ESP_OK;
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
    *state = g_host_onboarding_pending ? ONBOARDING_SERVICE_PENDING :
             ONBOARDING_SERVICE_COMPLETED;
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

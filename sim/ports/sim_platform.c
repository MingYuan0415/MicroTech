/** @file Remaining IDF surface implementations: error names and the app
 *  version descriptor (injectable for CI scenarios).
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_err.h"

static esp_app_desc_t s_app_desc =
{
    .magic_word = 0xABCD5431U,
    .version = "sim-0.0.0",
    .idf_ver = "host",
    .project_name = "microtech_sim",
    .time = "00:00:00",
    .date = "Jan  1 2026",
};

const char *esp_err_to_name(esp_err_t code)
{
    switch (code)
    {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_INVALID_VERSION:
        return "ESP_ERR_INVALID_VERSION";
    case ESP_ERR_INVALID_RESPONSE:
        return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC:
        return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_NOT_FINISHED:
        return "ESP_ERR_NOT_FINISHED";
    case ESP_ERR_NVS_NOT_INITIALIZED:
        return "ESP_ERR_NVS_NOT_INITIALIZED";
    case ESP_ERR_NVS_NOT_FOUND:
        return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return "ESP_ERR_NVS_NO_FREE_PAGES";
    default:
        return "UNKNOWN_ERROR";
    }
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_app_desc;
}

void sim_app_desc_set_version(const char *version)
{
    if (version != NULL)
    {
        strncpy(s_app_desc.version, version, sizeof(s_app_desc.version) - 1U);
        s_app_desc.version[sizeof(s_app_desc.version) - 1U] = '\0';
    }
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return 0;
}

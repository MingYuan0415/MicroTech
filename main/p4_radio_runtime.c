#include "p4_radio_runtime.h"

#include "sdkconfig.h"

#if MT_PRODUCT_HOSTED_RADIO

#include <errno.h>

#include "esp_hosted.h"
#include "network_runtime.h"

#define DBG_TAG "p4_radio"
#include "mt_log.h"

static bool s_initialized;
static bool s_ready;

static esp_err_t _hosted_result(int result, const char *operation)
{
    if (result == 0)
    {
        return ESP_OK;
    }
    LOG_E("%s failed: %d", operation, result);
    return result == -ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

esp_err_t p4_radio_runtime_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = network_runtime_init();
    if (result != ESP_OK)
    {
        LOG_E("network foundation init failed: %s", esp_err_to_name(result));
        return result;
    }

    result = _hosted_result(esp_hosted_init(), "esp_hosted_init");
    if (result != ESP_OK)
    {
        return result;
    }
    s_initialized = true;

    result = _hosted_result(esp_hosted_connect_to_slave(),
                            "esp_hosted_connect_to_slave");
    if (result != ESP_OK)
    {
        (void)esp_hosted_deinit();
        s_initialized = false;
        return result;
    }

    s_ready = true;
    /* esp_hosted's init event already prints the host/coprocessor version
     * match; a second synchronous RPC here would only add up to its 5 s
     * timeout to the boot path. */
    LOG_I("hosted SDIO transport ready");
    return ESP_OK;
}

esp_err_t p4_radio_runtime_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }
    s_ready = false;
    const esp_err_t result = _hosted_result(esp_hosted_deinit(),
                                            "esp_hosted_deinit");
    s_initialized = false;
    return result;
}

bool p4_radio_runtime_is_ready(void)
{
    return s_initialized && s_ready;
}

#else

esp_err_t p4_radio_runtime_init(void)
{
    return ESP_OK;
}

esp_err_t p4_radio_runtime_deinit(void)
{
    return ESP_OK;
}

bool p4_radio_runtime_is_ready(void)
{
    return false;
}

#endif /* MT_PRODUCT_HOSTED_RADIO */

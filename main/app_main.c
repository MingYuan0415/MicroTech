#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_runtime.h"

void app_main(void)
{
    const esp_err_t result = app_runtime_start();
    if (result != ESP_OK)
    {
        LOG_E("runtime startup failed: %s", esp_err_to_name(result));
    }
}

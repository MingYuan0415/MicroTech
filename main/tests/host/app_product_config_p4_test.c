#include "app_product_config.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    const app_product_config_t *config = app_product_config_get();
    assert(config != NULL);
    assert(strcmp(MT_PRODUCT_NAME, "p4_reference") == 0);
    assert(MT_PRODUCT_SYSTEM_PM_SUPPORTED == 0);
    assert(MT_PRODUCT_HOSTED_RADIO == 1);
    assert(config->system_pm_supported == false);
    assert(config->radio_enabled == true);
    assert(config->radio_optional == true);
    assert(strcmp(config->device_info.panel, "320 x 480 IPS") == 0);
    assert(strcmp(config->device_info.platform, "ESP32-P4") == 0);
    assert(config->connectivity.no_profile_radio_window_ms == 300000U);
    return 0;
}

#include "app_product_config.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    const app_product_config_t *config = app_product_config_get();
    assert(config != NULL);
    assert(strcmp(MT_PRODUCT_NAME, "s3_reference") == 0);
    assert(MT_PRODUCT_SYSTEM_PM_SUPPORTED == 1);
    assert(MT_PRODUCT_HOSTED_RADIO == 0);
    assert(config->system_pm_supported == true);
    assert(config->radio_enabled == true);
    assert(config->radio_optional == false);
    assert(strcmp(config->device_info.panel, "368 x 448 AMOLED") == 0);
    assert(strcmp(config->device_info.platform, "ESP32-S3") == 0);
#if !defined(MT_PRODUCT_HAS_LEVEL_APP)
#error "The S3 reference product must include the Level App"
#endif
    return 0;
}

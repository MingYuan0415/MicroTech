#define DBG_TAG "main"
#include "mt_log.h"

#include "app_runtime.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <inttypes.h>

static const char *_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "EFUSE";
    case ESP_RST_PWR_GLITCH:
        return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
        return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static void _log_boot_identity(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    const esp_partition_t *partition = esp_ota_get_running_partition();
    const uint64_t boot_id = ((uint64_t)esp_random() << 32) ^
                             (uint64_t)esp_random() ^
                             (uint64_t)esp_timer_get_time();

    LOG_I("boot id=%016" PRIx64 " reason=%s version=%s elf=%s partition=%s",
          boot_id,
          _reset_reason_name(esp_reset_reason()),
          description != NULL ? description->version : "unknown",
          esp_app_get_elf_sha256_str(),
          partition != NULL ? partition->label : "unknown");
}

void app_main(void)
{
    _log_boot_identity();
    const esp_err_t result = app_runtime_start();
    if (result != ESP_OK)
    {
        LOG_E("runtime startup failed: %s", esp_err_to_name(result));
    }
}

#include "app_product_config.h"

#include "freertos/FreeRTOS.h"

#define APP_PRODUCT_IMU_TASK_PRIORITY         6U
#define APP_PRODUCT_SYSTEM_PM_TASK_PRIORITY   5U
#define APP_PRODUCT_APP_CONTROL_TASK_PRIORITY 5U
#define APP_PRODUCT_TIME_TASK_PRIORITY        4U
#define APP_PRODUCT_POWER_TASK_PRIORITY       4U
#define APP_PRODUCT_CONNECTIVITY_TASK_PRIORITY 4U
#define APP_PRODUCT_WIFI_TASK_PRIORITY        4U
#define APP_PRODUCT_DEVICE_LINK_TASK_PRIORITY 4U
#define APP_PRODUCT_WEATHER_TASK_PRIORITY      4U

/* Recent-tasks keeps at most one pinned home task plus three business tasks
 * so the fixed three-slot PSRAM preview cache bounds retained thumbnails. */
#define APP_PRODUCT_MAX_RESIDENT_APPS          4U

#define APP_PRODUCT_PRIORITY_VALID(priority) \
    ((priority) > 0U && (priority) < configMAX_PRIORITIES)

_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_IMU_TASK_PRIORITY),
               "invalid product IMU task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_SYSTEM_PM_TASK_PRIORITY),
               "invalid product system-PM task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_APP_CONTROL_TASK_PRIORITY),
               "invalid product app-control task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_TIME_TASK_PRIORITY),
               "invalid product time task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_POWER_TASK_PRIORITY),
               "invalid product power task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(
                   APP_PRODUCT_CONNECTIVITY_TASK_PRIORITY),
               "invalid product connectivity task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_WIFI_TASK_PRIORITY),
               "invalid product Wi-Fi task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(
                   APP_PRODUCT_DEVICE_LINK_TASK_PRIORITY),
               "invalid product device-link task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_WEATHER_TASK_PRIORITY),
               "invalid product weather task priority");

static const app_product_config_t s_product_config =
{
    .audio = {
        .stream = {
            .sample_rate_hz = 16000U,
            .bits_per_sample = 16U,
            .channels = 2U,
            .mclk_multiple = 384U,
        },
        .volume_percent = 60U,
        .muted = false,
        .pa_enabled = true,
    },
    .sd = {
        .mount_path = "/sdcard",
        .max_files = 5,
        .allocation_unit_size = 16U * 1024U,
    },
    .imu = {
        .sample_rate_hz = 100U,
        .task_priority = APP_PRODUCT_IMU_TASK_PRIORITY,
    },
    .power = {
        .poll_interval_ms = 5000U,
        .irq_poll_interval_ms = 100U,
        .task_priority = APP_PRODUCT_POWER_TASK_PRIORITY,
    },
    .time = {
        .timezone = "CST-8",
        .sntp_server = "pool.ntp.org",
        .task_priority = APP_PRODUCT_TIME_TASK_PRIORITY,
    },
    .weather = {
        .server_base_url = CONFIG_MAIN_WEATHER_SERVER_BASE_URL,
        .device_token = CONFIG_MAIN_WEATHER_DEVICE_TOKEN,
        .cache_directory = "/data",
        .task_priority = APP_PRODUCT_WEATHER_TASK_PRIORITY,
        .current_refresh_seconds = 20U * 60U,
        .alerts_refresh_seconds = 10U * 60U,
        .hourly_refresh_seconds = 60U * 60U,
        .daily_refresh_seconds = 4U * 60U * 60U,
        .manual_refresh_min_seconds = 60U,
        .allow_private_http = false,
    },
    .connectivity = {
        .task_priority = APP_PRODUCT_CONNECTIVITY_TASK_PRIORITY,
        .wifi_task_priority = APP_PRODUCT_WIFI_TASK_PRIORITY,
    },
    .device_link = {
        .runtime_port = NULL,
        .task_priority = APP_PRODUCT_DEVICE_LINK_TASK_PRIORITY,
        .window_ms = 10U * 60U * 1000U,
    },
    .system_pm_task_priority = APP_PRODUCT_SYSTEM_PM_TASK_PRIORITY,
    .app_control_task_priority = APP_PRODUCT_APP_CONTROL_TASK_PRIORITY,
    .app_max_resident_apps = APP_PRODUCT_MAX_RESIDENT_APPS,
    .app_resident_policy = APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND,
};

const app_product_config_t *app_product_config_get(void)
{
    return &s_product_config;
}

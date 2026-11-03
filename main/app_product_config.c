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
#define APP_PRODUCT_MUSIC_TASK_PRIORITY       4U
#define APP_PRODUCT_MUSIC_AUDIO_PRIORITY      5U
#define APP_PRODUCT_CHORE_TASK_PRIORITY        4U

/* A user-opened Wi-Fi switch with no saved network keeps the radio available
 * for this long; without a completed connection it turns itself off again. */
#define APP_PRODUCT_WIFI_NO_PROFILE_WINDOW_MS  (5U * 60U * 1000U)

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
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_MUSIC_TASK_PRIORITY),
               "invalid product music task priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_MUSIC_AUDIO_PRIORITY),
               "invalid product music audio priority");
_Static_assert(APP_PRODUCT_PRIORITY_VALID(APP_PRODUCT_CHORE_TASK_PRIORITY),
               "invalid product chore task priority");

static const app_product_config_t s_product_config =
{
    .audio = {
        .stream = {
            .sample_rate_hz = 16000U,
            .bits_per_sample = 16U,
            .channels = 2U,
            .mclk_multiple = 384U,
        },
        /* 0..100 maps to -50..0 dB after the ES8311 hardware gain, so 100 is
         * unity. Anything below it stacks with the -6 dB PA trim and leaves a
         * voice memo unusable next to the device's own playback. */
        .volume_percent = 100U,
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
    .music = {
        .api_task_priority = APP_PRODUCT_MUSIC_TASK_PRIORITY,
        .audio_task_priority = APP_PRODUCT_MUSIC_AUDIO_PRIORITY,
        .default_quality = MUSIC_QUALITY_STANDARD,
    },
    .chore = {
        .task_priority = APP_PRODUCT_CHORE_TASK_PRIORITY,
        .warning_duration_ms = 500U,
    },
#if CONFIG_IDF_TARGET_ESP32P4
    .device_info = {
        .panel = "320 x 480 IPS",
        .platform = "ESP32-P4",
    },
    /* The ESP32-P4 board reaches Wi-Fi and BLE through the on-board ESP32-C6
     * over the hosted SDIO link. A link that cannot be brought up must not
     * take the display and local services down, so the radio stays optional.
     * The ESP32-S3 board keeps its native Wi-Fi/BLE controllers. */
    .radio_enabled = true,
    .radio_optional = true,
    .system_pm_supported = false,
#else
    .device_info = {
        .panel = "368 x 448 AMOLED",
        .platform = "ESP32-S3",
    },
    .radio_enabled = true,
    .radio_optional = false,
    .system_pm_supported = true,
#endif
    .connectivity = {
        .task_priority = APP_PRODUCT_CONNECTIVITY_TASK_PRIORITY,
        .wifi_task_priority = APP_PRODUCT_WIFI_TASK_PRIORITY,
        .no_profile_radio_window_ms = APP_PRODUCT_WIFI_NO_PROFILE_WINDOW_MS,
    },
    .device_link = {
        .runtime_port = NULL,
        .task_priority = APP_PRODUCT_DEVICE_LINK_TASK_PRIORITY,
        .window_ms = 120U * 1000U,
        .firmware_major = 0U,
        .firmware_minor = 1U,
        .firmware_patch = 0U,
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

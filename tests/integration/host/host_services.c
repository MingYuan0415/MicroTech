#include "apps_integration_runtime.h"

#include "audio_service.h"
#include "event_bus.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "host_wifi_service.h"
#include "imu_service.h"
#include "power_service.h"
#include "sd_storage_service.h"
#include "time_service.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

EVENT_BUS_DEFINE_ID(POWER_SERVICE_MSG);
EVENT_BUS_DEFINE_ID(IMU_SERVICE_MSG);
EVENT_BUS_DEFINE_ID(TIME_SERVICE_MSG);

static uint8_t s_brightness = 192;
static int32_t s_screen_timeout_ms = 30000;
static int32_t s_standby_timeout_ms = 5000;
static atomic_bool s_power_available;
static atomic_bool s_time_available;
static atomic_bool s_imu_available;
static atomic_bool s_audio_available;
static atomic_bool s_storage_available;
static atomic_uint s_random_nonce = 0x13579BDFU;
static atomic_uint s_imu_sequence = 1U;
static atomic_int s_time_quality = TIME_SERVICE_QUALITY_RTC;
static atomic_bool s_time_sync_owned;
static atomic_bool s_time_alarm_enabled;
static atomic_bool s_time_sync_blocked;
static atomic_bool s_time_sync_entered;
static atomic_int s_audio_state = AUDIO_SERVICE_STATE_READY;
static atomic_uint s_audio_volume = 60U;
static atomic_bool s_audio_muted;
static atomic_bool s_audio_pa_enabled;
static atomic_uint s_audio_read_count;
static atomic_uint s_audio_set_volume_count;
static atomic_int s_audio_read_peak;
static atomic_bool s_audio_fail_next_volume;
static const esp_app_desc_t s_app_description =
{
    .version = "test-version",
    .project_name = "microtech-test",
    .time = "12:00:00",
    .date = "Jul 19 2026",
    .idf_ver = "v6.0-test",
};

void host_runtime_reset(void)
{
    s_brightness = 192;
    s_screen_timeout_ms = 30000;
    s_standby_timeout_ms = 5000;
    atomic_store(&s_power_available, true);
    atomic_store(&s_time_available, true);
    atomic_store(&s_imu_available, true);
    atomic_store(&s_audio_available, true);
    atomic_store(&s_storage_available, true);
    atomic_store(&s_random_nonce, 0x13579BDFU);
    atomic_store(&s_imu_sequence, 1U);
    atomic_store(&s_time_quality, TIME_SERVICE_QUALITY_RTC);
    atomic_store(&s_time_sync_owned, false);
    atomic_store(&s_time_alarm_enabled, false);
    atomic_store(&s_time_sync_blocked, false);
    atomic_store(&s_time_sync_entered, false);
    atomic_store(&s_audio_state, AUDIO_SERVICE_STATE_READY);
    atomic_store(&s_audio_volume, 60U);
    atomic_store(&s_audio_muted, false);
    atomic_store(&s_audio_pa_enabled, false);
    atomic_store(&s_audio_read_count, 0U);
    atomic_store(&s_audio_set_volume_count, 0U);
    atomic_store(&s_audio_read_peak, 512);
    atomic_store(&s_audio_fail_next_volume, false);
    host_lv_reset();
    host_wifi_service_reset();
}

void host_optional_services_set_available(bool available)
{
    atomic_store(&s_power_available, available);
    atomic_store(&s_time_available, available);
    atomic_store(&s_imu_available, available);
    atomic_store(&s_audio_available, available);
    atomic_store(&s_storage_available, available);
    if (!available)
    {
        atomic_store(&s_time_quality, TIME_SERVICE_QUALITY_INVALID);
    }
    else if (atomic_load(&s_time_quality) == TIME_SERVICE_QUALITY_INVALID)
    {
        atomic_store(&s_time_quality, TIME_SERVICE_QUALITY_RTC);
    }
}

uint8_t host_audio_volume(void)
{
    return (uint8_t)atomic_load(&s_audio_volume);
}

unsigned host_audio_read_count(void)
{
    return atomic_load(&s_audio_read_count);
}

unsigned host_audio_set_volume_count(void)
{
    return atomic_load(&s_audio_set_volume_count);
}

void host_audio_set_read_peak(int16_t peak)
{
    atomic_store(&s_audio_read_peak, peak);
}

void host_audio_fail_next_volume(void)
{
    atomic_store(&s_audio_fail_next_volume, true);
}

bool host_time_alarm_is_enabled(void)
{
    return atomic_load(&s_time_alarm_enabled);
}

bool host_time_sync_is_owned(void)
{
    return atomic_load(&s_time_sync_owned);
}

void host_time_sync_set_blocked(bool blocked)
{
    if (blocked)
    {
        atomic_store(&s_time_sync_entered, false);
    }
    atomic_store(&s_time_sync_blocked, blocked);
}

bool host_time_sync_request_entered(void)
{
    return atomic_load(&s_time_sync_entered);
}

esp_err_t host_time_publish_alarm(uint32_t sequence)
{
    if (sequence == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const time_service_alarm_event_t event =
    {
        .sequence = sequence,
    };
    return event_bus_publish(TIME_SERVICE_MSG,
                             TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM,
                             &event, sizeof(event), 0U);
}

esp_err_t power_service_get_snapshot(power_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_power_available))
    {
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = (power_service_snapshot_t)
    {
        .info =
        {
            .battery_voltage_mv = 3910,
            .battery_percent = 78,
            .is_charging = false,
            .is_vbus_connected = false,
        },
        .sampled_at_ms = 123456,
        .valid = true,
    };
    return ESP_OK;
}

esp_err_t time_service_get_local(struct tm *local_time)
{
    if (local_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_time_available))
    {
        return ESP_ERR_INVALID_STATE;
    }
    *local_time = (struct tm)
    {
        .tm_sec = 0,
        .tm_min = 30,
        .tm_hour = 8,
        .tm_mday = 16,
        .tm_mon = 6,
        .tm_year = 126,
        .tm_wday = 4,
        .tm_yday = 196,
        .tm_isdst = 0,
    };
    return ESP_OK;
}

esp_err_t time_service_get_utc(struct tm *utc_time)
{
    if (utc_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_time_available))
    {
        return ESP_ERR_INVALID_STATE;
    }
    *utc_time = (struct tm)
    {
        .tm_sec = 0,
        .tm_min = 30,
        .tm_hour = 0,
        .tm_mday = 16,
        .tm_mon = 6,
        .tm_year = 126,
        .tm_wday = 4,
        .tm_yday = 196,
        .tm_isdst = 0,
    };
    return ESP_OK;
}

time_service_quality_t time_service_get_quality(void)
{
    if (!atomic_load(&s_time_available))
    {
        return TIME_SERVICE_QUALITY_INVALID;
    }
    return (time_service_quality_t)atomic_load(&s_time_quality);
}

esp_err_t time_service_request_sync(void)
{
    if (!atomic_load(&s_time_available))
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_time_sync_entered, true);
    while (atomic_load(&s_time_sync_blocked))
    {
        (void)usleep(1000U);
    }
    atomic_store(&s_time_sync_owned, true);
    atomic_store(&s_time_quality, TIME_SERVICE_QUALITY_NTP);
    return ESP_OK;
}

esp_err_t time_service_cancel_sync(void)
{
    atomic_store(&s_time_sync_owned, false);
    return ESP_OK;
}

esp_err_t time_service_alarm_configure(
    const time_service_alarm_config_t *config)
{
    if (config == NULL ||
            (!config->match_second && !config->match_minute &&
             !config->match_hour && !config->match_day &&
             !config->match_weekday))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_time_available))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    atomic_store(&s_time_alarm_enabled, true);
    return ESP_OK;
}

esp_err_t time_service_alarm_disable(void)
{
    atomic_store(&s_time_alarm_enabled, false);
    return ESP_OK;
}

esp_err_t time_service_alarm_get_status(time_service_alarm_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_time_available))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *status = (time_service_alarm_status_t)
    {
        .enabled = atomic_load(&s_time_alarm_enabled),
        .pending = false,
        .interrupt_active = false,
    };
    return ESP_OK;
}

esp_err_t imu_service_get_snapshot(imu_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_imu_available))
    {
        *snapshot = (imu_service_snapshot_t)
        {
            0
        };
        return ESP_OK;
    }
    const uint32_t sequence = atomic_fetch_add(&s_imu_sequence, 1U);
    *snapshot = (imu_service_snapshot_t)
    {
        .sample =
        {
            .acceleration_mps2 =
            {
                .x = 0.35F,
                .y = -0.20F,
                .z = 9.80F,
            },
            .angular_velocity_dps =
            {
                .x = 1.0F,
                .y = -2.0F,
                .z = 0.5F,
            },
            .temperature_c = 26.5F,
            .data_ready = true,
            .sampled_at_us = (int64_t)sequence * 50000,
            .sequence = sequence,
        },
        .sampled_at_us = (int64_t)sequence * 50000,
        .sequence = sequence,
        .valid = true,
        .available = true,
    };
    return ESP_OK;
}

imu_service_state_t imu_service_get_state(void)
{
    return IMU_SERVICE_STATE_RUNNING;
}

audio_service_config_t audio_service_get_default_config(void)
{
    return (audio_service_config_t)
    {
        .sample_rate_hz = 16000U,
        .bits_per_sample = 16U,
        .channels = 2U,
        .mclk_multiple = 384U,
    };
}

bool audio_service_is_available(void)
{
    return atomic_load(&s_audio_available);
}

audio_service_state_t audio_service_get_state(void)
{
    return (audio_service_state_t)atomic_load(&s_audio_state);
}

esp_err_t audio_service_start(void)
{
    const int state = atomic_load(&s_audio_state);
    if (state != AUDIO_SERVICE_STATE_READY &&
            state != AUDIO_SERVICE_STATE_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_audio_state, AUDIO_SERVICE_STATE_RUNNING);
    return ESP_OK;
}

esp_err_t audio_service_stop(void)
{
    atomic_store(&s_audio_pa_enabled, false);
    atomic_store(&s_audio_state, AUDIO_SERVICE_STATE_READY);
    return ESP_OK;
}

esp_err_t audio_service_write(void *data, size_t bytes, size_t *written,
                              uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (written != NULL)
    {
        *written = 0U;
    }
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&s_audio_state) != AUDIO_SERVICE_STATE_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)usleep(1000U);
    if (written != NULL)
    {
        *written = bytes;
    }
    return ESP_OK;
}

esp_err_t audio_service_read(void *data, size_t bytes, size_t *read,
                             uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (read != NULL)
    {
        *read = 0U;
    }
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&s_audio_state) != AUDIO_SERVICE_STATE_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)usleep(1000U);
    memset(data, 0, bytes);
    if (bytes >= sizeof(int16_t))
    {
        const int16_t peak = (int16_t)atomic_load(&s_audio_read_peak);
        memcpy(data, &peak, sizeof(peak));
    }
    if (read != NULL)
    {
        *read = bytes;
    }
    atomic_fetch_add(&s_audio_read_count, 1U);
    return ESP_OK;
}

esp_err_t audio_service_set_volume(uint8_t percent)
{
    if (percent > 100U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_fetch_add(&s_audio_set_volume_count, 1U);
    if (atomic_exchange(&s_audio_fail_next_volume, false))
    {
        return ESP_FAIL;
    }
    atomic_store(&s_audio_volume, percent);
    return ESP_OK;
}

esp_err_t audio_service_get_volume(uint8_t *percent)
{
    if (percent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *percent = (uint8_t)atomic_load(&s_audio_volume);
    return ESP_OK;
}

esp_err_t audio_service_set_mute(bool muted)
{
    atomic_store(&s_audio_muted, muted);
    return ESP_OK;
}

esp_err_t audio_service_get_mute(bool *muted)
{
    if (muted == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *muted = atomic_load(&s_audio_muted);
    return ESP_OK;
}

esp_err_t audio_service_set_pa(bool enabled)
{
    atomic_store(&s_audio_pa_enabled, enabled);
    return ESP_OK;
}

esp_err_t app_manager_screen_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    return ESP_OK;
}

esp_err_t app_manager_screen_set_brightness_temp(uint8_t brightness)
{
    (void)brightness;
    return ESP_OK;
}

uint8_t app_manager_screen_get_brightness(void)
{
    return s_brightness;
}

esp_err_t app_manager_pm_request_screen_off(void)
{
    return ESP_OK;
}

int32_t app_manager_pm_get_timeout_ms(void)
{
    return s_screen_timeout_ms;
}

esp_err_t app_manager_pm_set_timeout_ms(int32_t timeout_ms)
{
    s_screen_timeout_ms = timeout_ms;
    return ESP_OK;
}

int32_t app_manager_pm_get_standby_delay_ms(void)
{
    return s_standby_timeout_ms;
}

esp_err_t app_manager_pm_set_standby_delay_ms(int32_t timeout_ms)
{
    s_standby_timeout_ms = timeout_ms;
    return ESP_OK;
}

bool sd_storage_service_is_mounted(void)
{
    return atomic_load(&s_storage_available);
}

const char *sd_storage_service_get_mount_path(void)
{
    return "/sdcard";
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total_bytes,
                           uint64_t *free_bytes)
{
    if (base_path == NULL || strcmp(base_path, "/sdcard") != 0 ||
            total_bytes == NULL || free_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *total_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    *free_bytes = 24ULL * 1024ULL * 1024ULL * 1024ULL;
    return ESP_OK;
}

uint32_t esp_random(void)
{
    return atomic_fetch_add(&s_random_nonce, 0x9E3779B9U);
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_app_description;
}

const char *esp_err_to_name(esp_err_t error)
{
    switch (error)
    {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FINISHED:
        return "ESP_ERR_NOT_FINISHED";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

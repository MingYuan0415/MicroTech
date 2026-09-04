#include "apps_integration_runtime.h"
#include "sim_sd_host.h"

#include "app_manager.h"
#include "audio_service.h"
#include "event_bus.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "host_connectivity_manager.h"
#include "host_device_link_service.h"
#include "host_factory_reset_service.h"
#include "imu_service.h"
#include "nv_storage.h"
#include "power_service.h"
#include "sd_storage_service.h"
#include "time_service.h"
#include "weather_service.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>


static uint8_t s_brightness = 192;
static int32_t s_screen_timeout_ms = 30000;
static int32_t s_standby_timeout_ms = 5000;
static atomic_bool s_power_available;
static atomic_bool s_time_available;
static atomic_bool s_imu_available;
static atomic_bool s_audio_available;

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
static atomic_uint s_weather_snapshot_index;
static atomic_uint s_weather_snapshot_leases;
static atomic_uint s_weather_refresh_count;
static atomic_int s_weather_refresh_result;
static atomic_int s_weather_state;
static atomic_uint s_weather_retry_after_seconds;
static weather_service_snapshot_t s_weather_snapshots[2];

void host_runtime_reset(void)
{
    s_brightness = 192;
    s_screen_timeout_ms = 30000;
    s_standby_timeout_ms = 5000;
    atomic_store(&s_power_available, true);
    atomic_store(&s_time_available, true);
    atomic_store(&s_imu_available, true);
    atomic_store(&s_audio_available, true);
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
    atomic_store(&s_weather_snapshot_index, 0U);
    atomic_store(&s_weather_snapshot_leases, 0U);
    atomic_store(&s_weather_refresh_count, 0U);
    atomic_store(&s_weather_refresh_result, ESP_OK);
    atomic_store(&s_weather_state, WEATHER_SERVICE_STATE_READY);
    atomic_store(&s_weather_retry_after_seconds, 0U);
    memset(s_weather_snapshots, 0, sizeof(s_weather_snapshots));
    for (size_t index = 0U; index < 2U; ++index)
    {
        weather_service_snapshot_t *snapshot = &s_weather_snapshots[index];
        snapshot->generation = index + 1U;
        snapshot->available_mask = WEATHER_SERVICE_DATA_LOCATION |
                                   WEATHER_SERVICE_DATA_CURRENT |
                                   WEATHER_SERVICE_DATA_ALERTS |
                                   WEATHER_SERVICE_DATA_HOURLY |
                                   WEATHER_SERVICE_DATA_DAILY;
        memcpy(snapshot->location.city, "Shenzhen", sizeof("Shenzhen"));
        memcpy(snapshot->location.region, "Guangdong", sizeof("Guangdong"));
        memcpy(snapshot->location.country, "CN", sizeof("CN"));
        memcpy(snapshot->location.timezone, "Asia/Shanghai",
               sizeof("Asia/Shanghai"));
        memcpy(snapshot->location.provider, "maxmind", sizeof("maxmind"));
        snapshot->location.available = true;
        snapshot->current.meta.available = true;
        snapshot->current.temperature_tenths_c = 312;
        snapshot->current.feels_like_tenths_c = 356;
        snapshot->current.observed_at.epoch_seconds = 1785888000;
        snapshot->current.observed_at.offset_minutes = 480;
        snapshot->current.meta.updated_at = snapshot->current.observed_at;
        snapshot->current.meta.fetched_at = snapshot->current.observed_at;
        snapshot->current.condition_code = 101U;
        memcpy(snapshot->current.condition_text, "多云", sizeof("多云"));
        snapshot->current.humidity_percent = 72U;
        snapshot->current.precipitation_tenths_mm = 18U;
        snapshot->current.wind_speed_tenths_kmh = 123U;
        memcpy(snapshot->current.wind_direction, "东南风", sizeof("东南风"));
        memcpy(snapshot->current.wind_scale, "3 级", sizeof("3 级"));
        snapshot->current.pressure_hpa = 1004U;
        snapshot->current.visibility_tenths_km = 185U;
        snapshot->hourly.meta.available = true;
        snapshot->hourly.count = 12U;
        snapshot->hourly.meta.updated_at = snapshot->current.observed_at;
        snapshot->hourly.meta.fetched_at = snapshot->current.observed_at;
        for (uint8_t hour = 0U; hour < snapshot->hourly.count; ++hour)
        {
            weather_service_hour_t *item = &snapshot->hourly.items[hour];
            item->forecast_at.epoch_seconds = 1785891600 +
                                              (int64_t)hour * 3600;
            item->forecast_at.offset_minutes = 480;
            item->temperature_tenths_c = (int16_t)(320 - (int16_t)hour * 3);
            item->condition_code = hour % 3U == 0U ? 100U : 101U;
            memcpy(item->condition_text, hour % 3U == 0U ? "晴" : "多云",
                   hour % 3U == 0U ? sizeof("晴") : sizeof("多云"));
            item->humidity_percent = (uint8_t)(65U + hour);
            item->precipitation_chance_percent = (uint8_t)(hour * 3U);
            item->precipitation_tenths_mm = (uint16_t)(hour * 2U);
            item->wind_speed_tenths_kmh = (uint16_t)(100U + hour * 4U);
            memcpy(item->wind_direction, "东南风", sizeof("东南风"));
        }
        snapshot->daily.meta.available = true;
        snapshot->daily.count = 7U;
        snapshot->daily.meta.updated_at = snapshot->current.observed_at;
        snapshot->daily.meta.fetched_at = snapshot->current.observed_at;
        static const char *const dates[] =
        {
            "2026-08-05", "2026-08-06", "2026-08-07", "2026-08-08",
            "2026-08-09", "2026-08-10", "2026-08-11",
        };
        for (uint8_t day = 0U; day < snapshot->daily.count; ++day)
        {
            weather_service_day_t *item = &snapshot->daily.items[day];
            memcpy(item->date, dates[day], sizeof(item->date));
            item->maximum_temperature_tenths_c =
                (int16_t)(340 - (int16_t)day * 4);
            item->minimum_temperature_tenths_c =
                (int16_t)(270 - (int16_t)day * 2);
            item->day_condition_code = day % 2U == 0U ? 100U : 305U;
            item->night_condition_code = day % 2U == 0U ? 150U : 305U;
            memcpy(item->day_condition_text,
                   day % 2U == 0U ? "晴" : "小雨",
                   day % 2U == 0U ? sizeof("晴") : sizeof("小雨"));
            memcpy(item->night_condition_text,
                   day % 2U == 0U ? "晴" : "小雨",
                   day % 2U == 0U ? sizeof("晴") : sizeof("小雨"));
            item->humidity_percent = (uint8_t)(65U + day);
            item->precipitation_tenths_mm = (uint16_t)(day * 12U);
            item->visibility_tenths_km = 180U;
            item->uv_index = (uint8_t)(7U - day);
        }
        snapshot->alerts.meta.available = true;
    }
    s_weather_snapshots[0].alerts.count = 2U;
    s_weather_snapshots[0].alerts.items[0].key = UINT64_C(0x1234);
    memcpy(s_weather_snapshots[0].alerts.items[0].title, "暴雨红色预警",
           sizeof("暴雨红色预警"));
    memcpy(s_weather_snapshots[0].alerts.items[0].type_name, "暴雨",
           sizeof("暴雨"));
    memcpy(s_weather_snapshots[0].alerts.items[0].severity, "severe",
           sizeof("severe"));
    memcpy(s_weather_snapshots[0].alerts.items[0].status, "active",
           sizeof("active"));
    memcpy(s_weather_snapshots[0].alerts.items[0].description,
           "预计未来三小时有强降雨。", sizeof("预计未来三小时有强降雨。"));
    memcpy(s_weather_snapshots[0].alerts.items[0].instruction,
           "请减少外出。", sizeof("请减少外出。"));
    s_weather_snapshots[0].alerts.items[0].starts_at.epoch_seconds =
        1785888000;
    s_weather_snapshots[0].alerts.items[0].starts_at.offset_minutes = 480;
    s_weather_snapshots[0].alerts.items[0].ends_at.epoch_seconds = 1785909600;
    s_weather_snapshots[0].alerts.items[0].ends_at.offset_minutes = 480;
    s_weather_snapshots[0].alerts.items[1].key = UINT64_C(0x5678);
    memcpy(s_weather_snapshots[0].alerts.items[1].title, "高温橙色预警",
           sizeof("高温橙色预警"));
    memcpy(s_weather_snapshots[0].alerts.items[1].type_name, "高温",
           sizeof("高温"));
    memcpy(s_weather_snapshots[0].alerts.items[1].severity, "moderate",
           sizeof("moderate"));
    memcpy(s_weather_snapshots[0].alerts.items[1].status, "active",
           sizeof("active"));
    memcpy(s_weather_snapshots[0].alerts.items[1].description,
           "预计午后最高气温超过三十七度。",
           sizeof("预计午后最高气温超过三十七度。"));
    memcpy(s_weather_snapshots[0].alerts.items[1].instruction,
           "请减少户外活动。", sizeof("请减少户外活动。"));
    s_weather_snapshots[0].alerts.items[1].starts_at.epoch_seconds =
        1785891600;
    s_weather_snapshots[0].alerts.items[1].starts_at.offset_minutes = 480;
    s_weather_snapshots[0].alerts.items[1].ends_at.epoch_seconds = 1785931200;
    s_weather_snapshots[0].alerts.items[1].ends_at.offset_minutes = 480;
    /* host LVGL fixture reset not applicable in the simulator */
    host_device_link_service_reset();
}


















void host_optional_services_set_available(bool available)
{
    atomic_store(&s_power_available, available);
    atomic_store(&s_time_available, available);
    atomic_store(&s_imu_available, available);
    atomic_store(&s_audio_available, available);
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

void host_audio_set_available(bool available)
{
    atomic_store(&s_audio_available, available);
}

void host_audio_fail_next_volume(void)
{
    atomic_store(&s_audio_fail_next_volume, true);
}

















esp_err_t audio_service_get_config(audio_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *config = (audio_service_config_t)
    {
        .sample_rate_hz = 16000U,
        .bits_per_sample = 16U,
        .channels = 2U,
        .mclk_multiple = 384U,
    };
    return ESP_OK;
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









static char s_sd_dir[PATH_MAX];
static char s_sd_recordings[PATH_MAX];
static bool s_sd_mounted;

static esp_err_t _host_sd_ensure_directory(const char *directory)
{
    char path[PATH_MAX];
    size_t length = strlen(directory);
    if (length == 0 || length >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(path, directory, length + 1U);
    for (char *cursor = path + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            *cursor = '\0';
            if (mkdir(path, 0775) != 0 && errno != EEXIST)
            {
                return ESP_FAIL;
            }
            *cursor = '/';
        }
    }
    return mkdir(path, 0775) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL;
}

esp_err_t host_sd_boot(const char *dir)
{
    if (dir == NULL || dir[0] != '/')
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(dir);
    if (length >= sizeof(s_sd_dir))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (_host_sd_ensure_directory(dir) != ESP_OK)
    {
        return ESP_FAIL;
    }
    memcpy(s_sd_dir, dir, length + 1U);
    if (snprintf(s_sd_recordings, sizeof(s_sd_recordings),
                 "%s/MicroTech/Recordings", s_sd_dir) >=
            (int)sizeof(s_sd_recordings))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    s_sd_mounted = true;
    if (strlen(s_sd_recordings) + 22U >= SIM_SD_NAME_MAX)
    {
        fprintf(stderr,
                "sim: sd dir too deep for recorder name buffer (%zu + names "
                "must fit %d bytes); recordings will not list\n",
                strlen(s_sd_recordings), SIM_SD_NAME_MAX);
    }
    return ESP_OK;
}

bool host_sd_set_mounted(bool mounted)
{
    char hidden[PATH_MAX];
    bool changed;

    if (s_sd_dir[0] == '\0' || mounted == s_sd_mounted)
    {
        return s_sd_mounted == mounted && s_sd_dir[0] != '\0';
    }
    if (snprintf(hidden, sizeof(hidden), "%s.umounted", s_sd_dir) >=
            (int)sizeof(hidden))
    {
        return false;
    }
    changed = mounted ? rename(hidden, s_sd_dir) == 0 :
              rename(s_sd_dir, hidden) == 0;
    if (!mounted && changed)
    {
        s_sd_mounted = false;
    }
    else if (mounted && changed)
    {
        s_sd_mounted = true;
    }
    else if (mounted && _host_sd_ensure_directory(s_sd_dir) == ESP_OK)
    {
        /* an empty volume was never renamed out: restore it by creation */
        s_sd_mounted = true;
        changed = true;
    }
    return changed;
}

bool host_sd_is_mounted(void)
{
    return s_sd_mounted && s_sd_dir[0] != '\0';
}

const char *host_sd_directory(void)
{
    return host_sd_is_mounted() ? s_sd_dir : NULL;
}

const char *host_sd_recordings_dir(void)
{
    return host_sd_is_mounted() ? s_sd_recordings : NULL;
}

esp_err_t host_sd_write_wav(const char *name, uint32_t seconds)
{
    const char *directory = host_sd_recordings_dir();
    char path[PATH_MAX];
    uint32_t data_size = seconds * 64000U;
    unsigned char header[44];
    static const uint32_t byte_rate = 64000U;
    FILE *file;

    if (directory == NULL || name == NULL || strchr(name, '/') != NULL ||
            strstr(name, "..") != NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
            (int)sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(header, "RIFF", 4U);
    header[4] = (unsigned char)((data_size + 36U) & 0xFFU);
    header[5] = (unsigned char)(((data_size + 36U) >> 8) & 0xFFU);
    header[6] = (unsigned char)(((data_size + 36U) >> 16) & 0xFFU);
    header[7] = (unsigned char)(((data_size + 36U) >> 24) & 0xFFU);
    memcpy(header + 8, "WAVEfmt ", 8U);
    header[16] = 16U;
    header[17] = 0U;
    header[18] = 0U;
    header[19] = 0U;
    header[20] = 1U;  /* PCM */
    header[21] = 0U;
    header[22] = 2U;  /* channels */
    header[23] = 0U;
    header[24] = (unsigned char)(16000U & 0xFFU);
    header[25] = (unsigned char)((16000U >> 8) & 0xFFU);
    header[26] = 0U;
    header[27] = 0U;
    header[28] = (unsigned char)(byte_rate & 0xFFU);
    header[29] = (unsigned char)((byte_rate >> 8) & 0xFFU);
    header[30] = (unsigned char)((byte_rate >> 16) & 0xFFU);
    header[31] = (unsigned char)((byte_rate >> 24) & 0xFFU);
    header[32] = 4U;  /* block align */
    header[33] = 0U;
    header[34] = 16U; /* bits */
    header[35] = 0U;
    memcpy(header + 36, "data", 4U);
    header[40] = (unsigned char)(data_size & 0xFFU);
    header[41] = (unsigned char)((data_size >> 8) & 0xFFU);
    header[42] = (unsigned char)((data_size >> 16) & 0xFFU);
    header[43] = (unsigned char)((data_size >> 24) & 0xFFU);
    file = fopen(path, "wb");
    if (file == NULL || fwrite(header, 1U, sizeof(header), file) !=
            sizeof(header))
    {
        (void)fclose(file);
        return ESP_FAIL;
    }
    if (data_size > 0U)
    {
        unsigned char silence[4096] = {0};
        uint32_t remaining = data_size;
        while (remaining > 0U)
        {
            size_t chunk = remaining > sizeof(silence) ? sizeof(silence) :
                           (size_t)remaining;
            if (fwrite(silence, 1U, chunk, file) != chunk)
            {
                (void)fclose(file);
                (void)remove(path);
                return ESP_FAIL;
            }
            remaining -= (uint32_t)chunk;
        }
    }
    if (fclose(file) != 0)
    {
        (void)remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t host_sd_clear_recordings(void)
{
    const char *directory = host_sd_recordings_dir();
    DIR *handle;
    struct dirent *entry;
    size_t removed = 0U;

    if (directory == NULL)
    {
        return 0U;
    }
    handle = opendir(directory);
    if (handle == NULL)
    {
        return 0U;
    }
    while ((entry = readdir(handle)) != NULL)
    {
        const size_t length = strlen(entry->d_name);
        if ((length > 4U && strcmp(entry->d_name + length - 4U, ".wav") == 0) ||
                (length > 5U &&
                 strcmp(entry->d_name + length - 5U, ".part") == 0))
        {
            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s", directory,
                         entry->d_name) < (int)sizeof(path) &&
                    remove(path) == 0)
            {
                ++removed;
            }
        }
    }
    (void)closedir(handle);
    return removed;
}

size_t host_sd_list_recordings(char names[][SIM_SD_NAME_MAX], size_t capacity)
{
    const char *directory = host_sd_recordings_dir();
    DIR *handle;
    struct dirent *entry;
    size_t count = 0U;

    if (names == NULL || directory == NULL)
    {
        return 0U;
    }
    handle = opendir(directory);
    if (handle == NULL)
    {
        return 0U;
    }
    while ((entry = readdir(handle)) != NULL && count < capacity)
    {
        size_t length = strlen(entry->d_name);
        if (length > 4U && length < SIM_SD_NAME_MAX &&
                strcmp(entry->d_name + length - 4U, ".wav") == 0)
        {
            memcpy(names[count], entry->d_name, length + 1U);
            ++count;
        }
    }
    (void)closedir(handle);
    return count;
}

bool sd_storage_service_is_mounted(void)
{
    return host_sd_is_mounted();
}

const char *sd_storage_service_get_mount_path(void)
{
    return host_sd_directory();
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total_bytes,
                           uint64_t *free_bytes)
{
    struct statvfs info;

    if (base_path == NULL || total_bytes == NULL || free_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (statvfs(base_path, &info) != 0)
    {
        return ESP_FAIL;
    }
    *total_bytes = (uint64_t)info.f_blocks * info.f_frsize;
    *free_bytes = (uint64_t)info.f_bavail * info.f_frsize;
    return ESP_OK;
}

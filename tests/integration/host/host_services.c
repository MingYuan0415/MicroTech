#include "apps_integration_runtime.h"

#include "app_manager.h"
#include "audio_service.h"
#include "event_bus.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "host_connectivity_manager.h"
#include "host_device_link_service.h"
#include "imu_service.h"
#include "power_service.h"
#include "sd_storage_service.h"
#include "time_service.h"
#include "weather_service.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

EVENT_BUS_DEFINE_ID(POWER_SERVICE_MSG);
EVENT_BUS_DEFINE_ID(IMU_SERVICE_MSG);
EVENT_BUS_DEFINE_ID(TIME_SERVICE_MSG);
EVENT_BUS_DEFINE_ID(WEATHER_SERVICE_MSG);

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
static atomic_uint s_weather_snapshot_index;
static atomic_uint s_weather_snapshot_leases;
static atomic_uint s_weather_refresh_count;
static atomic_int s_weather_refresh_result;
static atomic_int s_weather_state;
static atomic_uint s_weather_retry_after_seconds;
static weather_service_snapshot_t s_weather_snapshots[2];
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
    host_lv_reset();
    host_connectivity_manager_reset();
    host_device_link_service_reset();
}

unsigned host_weather_refresh_count(void)
{
    return atomic_load(&s_weather_refresh_count);
}

unsigned host_weather_snapshot_lease_count(void)
{
    return atomic_load(&s_weather_snapshot_leases);
}

void host_weather_set_refresh_result(esp_err_t result)
{
    atomic_store(&s_weather_refresh_result, result);
}

esp_err_t host_weather_publish(bool with_alert)
{
    unsigned index = with_alert ? 0U : 1U;
    atomic_store_explicit(&s_weather_snapshot_index, index,
                          memory_order_release);
    const weather_service_event_t event =
    {
        .generation = s_weather_snapshots[index].generation,
        .changed_mask = WEATHER_SERVICE_DATA_ALERTS,
        .state = (weather_service_state_t)atomic_load(&s_weather_state),
        .failure = WEATHER_SERVICE_FAILURE_NONE,
    };
    return event_bus_publish(WEATHER_SERVICE_MSG,
                             WEATHER_SERVICE_MSG_SUB_TYPE_SNAPSHOT,
                             &event, sizeof(event),
                             EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

void host_weather_set_available_mask(uint32_t available_mask)
{
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    weather_service_snapshot_t *snapshot = &s_weather_snapshots[index];
    snapshot->available_mask = available_mask;
    snapshot->location.available =
        (available_mask & WEATHER_SERVICE_DATA_LOCATION) != 0U;
    snapshot->current.meta.available =
        (available_mask & WEATHER_SERVICE_DATA_CURRENT) != 0U;
    snapshot->alerts.meta.available =
        (available_mask & WEATHER_SERVICE_DATA_ALERTS) != 0U;
    snapshot->hourly.meta.available =
        (available_mask & WEATHER_SERVICE_DATA_HOURLY) != 0U;
    snapshot->daily.meta.available =
        (available_mask & WEATHER_SERVICE_DATA_DAILY) != 0U;
}

void host_weather_set_current_freshness(bool stale, bool expired)
{
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    s_weather_snapshots[index].current.meta.stale = stale;
    s_weather_snapshots[index].current.meta.expired = expired;
}

void host_weather_set_alert_freshness(bool stale, bool expired)
{
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    s_weather_snapshots[index].alerts.meta.stale = stale;
    s_weather_snapshots[index].alerts.meta.expired = expired;
}

void host_weather_set_city(const char *city)
{
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    weather_service_snapshot_t *snapshot = &s_weather_snapshots[index];
    (void)snprintf(snapshot->location.city, sizeof(snapshot->location.city),
                   "%s", city == NULL ? "" : city);
}

void host_weather_set_district(const char *district)
{
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    weather_service_snapshot_t *snapshot = &s_weather_snapshots[index];
    (void)snprintf(snapshot->location.district,
                   sizeof(snapshot->location.district), "%s",
                   district == NULL ? "" : district);
}

void host_weather_set_layout_extremes(bool enabled)
{
    for (size_t index = 0U; index < 2U; ++index)
    {
        weather_service_snapshot_t *snapshot = &s_weather_snapshots[index];
        snapshot->current.feels_like_tenths_c = enabled ? -1000 : 356;
        snapshot->current.precipitation_tenths_mm = enabled ? UINT16_MAX : 18U;
        snapshot->current.wind_speed_tenths_kmh = enabled ? 10000U : 123U;
        snapshot->current.visibility_tenths_km = enabled ? 10000U : 185U;
        (void)snprintf(snapshot->current.condition_text,
                       sizeof(snapshot->current.condition_text), "%s",
                       enabled ? "雷阵雨伴有冰雹和大风" : "多云");

        snapshot->hourly.items[11].temperature_tenths_c = enabled ? -1000 : 287;
        weather_service_day_t *today = &snapshot->daily.items[0];
        today->maximum_temperature_tenths_c = enabled ? 1000 : 340;
        today->minimum_temperature_tenths_c = enabled ? -1000 : 270;

        weather_service_day_t *long_day = &snapshot->daily.items[1];
        (void)snprintf(long_day->day_condition_text,
                       sizeof(long_day->day_condition_text), "%s",
                       enabled ? "雷阵雨伴有冰雹" : "小雨");
        (void)snprintf(long_day->night_condition_text,
                       sizeof(long_day->night_condition_text), "%s",
                       enabled ? "雷阵雨伴有冰雹" : "小雨");
        long_day->precipitation_tenths_mm = enabled ? UINT16_MAX : 12U;
        long_day->uv_index = enabled ? 100U : 6U;
    }
}

void host_weather_set_location_reused(bool reused)
{
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    s_weather_snapshots[index].location.reused = reused;
}

void host_weather_set_service_state(int state,
                                    uint32_t retry_after_seconds)
{
    atomic_store(&s_weather_state, state);
    atomic_store(&s_weather_retry_after_seconds, retry_after_seconds);
}

esp_err_t weather_service_get_status(
    weather_service_status_snapshot_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    *status = (weather_service_status_snapshot_t)
    {
        .generation = s_weather_snapshots[index].generation,
        .state = (weather_service_state_t)atomic_load(&s_weather_state),
        .failure = WEATHER_SERVICE_FAILURE_NONE,
        .available_mask = s_weather_snapshots[index].available_mask,
        .initialized = true,
        .configured = true,
        .network_ready = true,
        .retry_after_seconds = atomic_load(&s_weather_retry_after_seconds),
    };
    return ESP_OK;
}

esp_err_t weather_service_snapshot_acquire(
    const weather_service_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned index = atomic_load_explicit(&s_weather_snapshot_index,
                                          memory_order_acquire);
    *snapshot = &s_weather_snapshots[index];
    atomic_fetch_add_explicit(&s_weather_snapshot_leases, 1U,
                              memory_order_relaxed);
    return ESP_OK;
}

void weather_service_snapshot_release(
    const weather_service_snapshot_t *snapshot)
{
    assert(snapshot == &s_weather_snapshots[0] ||
           snapshot == &s_weather_snapshots[1]);
    unsigned previous = atomic_fetch_sub_explicit(
                            &s_weather_snapshot_leases, 1U,
                            memory_order_relaxed);
    assert(previous > 0U);
}

esp_err_t weather_service_request_refresh(void)
{
    atomic_fetch_add(&s_weather_refresh_count, 1U);
    return atomic_load(&s_weather_refresh_result);
}

esp_err_t app_manager_get_image(uint32_t semantic_id,
                                const lv_image_dsc_t **image)
{
    if (semantic_id == 0U || image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    return ESP_ERR_NOT_FOUND;
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

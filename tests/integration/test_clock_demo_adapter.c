#include "clock_demo_adapter.h"

#include "esp_heap_caps.h"
#include "host_freertos.h"
#include "time_service.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_WAIT_ATTEMPTS 2000U

typedef struct fake_time_service
{
    pthread_mutex_t lock;
    time_service_alarm_config_t last_alarm;
    esp_err_t next_cancel_result;
    esp_err_t next_disable_result;
    unsigned sync_request_count;
    unsigned sync_cancel_count;
    unsigned alarm_configure_count;
    unsigned alarm_disable_count;
    bool sync_active;
    bool alarm_enabled;
} fake_time_service_t;

static fake_time_service_t s_time =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _assert_no_invalid_worker_delete(void)
{
    assert(host_caps_task_wrong_delete_count() == 0U);
    assert(host_caps_task_self_delete_count() == 0U);
}

static size_t _assert_psram_worker(void)
{
    const UBaseType_t required = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    assert(host_caps_task_count() == 1U);
    assert((host_last_task_stack_caps() & required) == required);
    _assert_no_invalid_worker_delete();
    return host_caps_task_owner_delete_count();
}

static void _assert_worker_retained(size_t owner_delete_count)
{
    assert(host_caps_task_count() == 1U);
    assert(host_caps_task_owner_delete_count() == owner_delete_count);
    _assert_no_invalid_worker_delete();
}

static void _assert_worker_released(size_t owner_delete_count)
{
    assert(host_caps_task_count() == 0U);
    assert(host_caps_task_owner_delete_count() == owner_delete_count + 1U);
    _assert_no_invalid_worker_delete();
}

static void _sleep_one_ms(void)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static void _fake_time_reset(void)
{
    (void)pthread_mutex_lock(&s_time.lock);
    memset(&s_time.last_alarm, 0, sizeof(s_time.last_alarm));
    s_time.next_cancel_result = ESP_OK;
    s_time.next_disable_result = ESP_OK;
    s_time.sync_request_count = 0U;
    s_time.sync_cancel_count = 0U;
    s_time.alarm_configure_count = 0U;
    s_time.alarm_disable_count = 0U;
    s_time.sync_active = false;
    s_time.alarm_enabled = false;
    (void)pthread_mutex_unlock(&s_time.lock);
}

static clock_demo_adapter_snapshot_t _wait_for_operation(
    clock_demo_adapter_t *adapter, bool sync_operation)
{
    clock_demo_adapter_snapshot_t snapshot = {0};
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        assert(clock_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK);
        const clock_demo_operation_state_t state = sync_operation ?
            snapshot.sync_state :
            snapshot.alarm_state;
        if (state == CLOCK_DEMO_OPERATION_DONE ||
                state == CLOCK_DEMO_OPERATION_FAILED)
        {
            return snapshot;
        }
        _sleep_one_ms();
    }
    assert(!"clock demo operation timed out");
    return snapshot;
}

static bool _alarm_matches_epoch(const time_service_alarm_config_t *config,
                                 time_t epoch)
{
    struct tm expected;
    return gmtime_r(&epoch, &expected) != NULL &&
           config->match_second &&
           config->second == (uint8_t)expected.tm_sec &&
           config->match_minute &&
           config->minute == (uint8_t)expected.tm_min &&
           config->match_hour &&
           config->hour == (uint8_t)expected.tm_hour &&
           config->match_day &&
           config->day == (uint8_t)expected.tm_mday &&
           config->match_weekday &&
           config->weekday == (uint8_t)expected.tm_wday;
}

static void _test_alarm_ten_seconds_from_now(void)
{
    _fake_time_reset();
    clock_demo_adapter_t adapter = {0};
    assert(clock_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _assert_psram_worker();

    const time_t started_at = time(NULL);
    assert(clock_demo_adapter_arm_alarm(&adapter) == ESP_OK);
    const clock_demo_adapter_snapshot_t snapshot =
        _wait_for_operation(&adapter, false);
    const time_t completed_at = time(NULL);
    assert(snapshot.alarm_state == CLOCK_DEMO_OPERATION_DONE);
    assert(snapshot.alarm_result == ESP_OK);
    assert(snapshot.alarm_owned);

    (void)pthread_mutex_lock(&s_time.lock);
    const time_service_alarm_config_t configured = s_time.last_alarm;
    const unsigned configure_count = s_time.alarm_configure_count;
    (void)pthread_mutex_unlock(&s_time.lock);
    assert(configure_count == 1U);

    bool target_matches = false;
    for (time_t current = started_at; current <= completed_at; ++current)
    {
        if (_alarm_matches_epoch(&configured, current + 10))
        {
            target_matches = true;
            break;
        }
    }
    assert(target_matches);
    assert(clock_demo_adapter_close(&adapter) == ESP_OK);
    _assert_worker_released(owner_delete_count);
    assert(!clock_demo_adapter_is_open(&adapter));
}

static void _test_external_alarm_is_not_overwritten(void)
{
    _fake_time_reset();
    (void)pthread_mutex_lock(&s_time.lock);
    s_time.alarm_enabled = true;
    (void)pthread_mutex_unlock(&s_time.lock);

    clock_demo_adapter_t adapter = {0};
    assert(clock_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _assert_psram_worker();
    assert(clock_demo_adapter_arm_alarm(&adapter) == ESP_OK);
    const clock_demo_adapter_snapshot_t snapshot =
        _wait_for_operation(&adapter, false);
    assert(snapshot.alarm_state == CLOCK_DEMO_OPERATION_FAILED);
    assert(snapshot.alarm_result == ESP_ERR_INVALID_STATE);
    assert(!snapshot.alarm_owned);
    assert(clock_demo_adapter_close(&adapter) == ESP_OK);
    _assert_worker_released(owner_delete_count);

    (void)pthread_mutex_lock(&s_time.lock);
    assert(s_time.alarm_enabled);
    assert(s_time.alarm_configure_count == 0U);
    assert(s_time.alarm_disable_count == 0U);
    (void)pthread_mutex_unlock(&s_time.lock);
}

static void _start_owned_sync_and_alarm(clock_demo_adapter_t *adapter)
{
    assert(clock_demo_adapter_request_sync(adapter) == ESP_OK);
    clock_demo_adapter_snapshot_t snapshot =
        _wait_for_operation(adapter, true);
    assert(snapshot.sync_state == CLOCK_DEMO_OPERATION_DONE);
    assert(snapshot.sync_owned);

    assert(clock_demo_adapter_arm_alarm(adapter) == ESP_OK);
    snapshot = _wait_for_operation(adapter, false);
    assert(snapshot.alarm_state == CLOCK_DEMO_OPERATION_DONE);
    assert(snapshot.alarm_owned);
}

static void _test_close_releases_owned_resources(void)
{
    _fake_time_reset();
    clock_demo_adapter_t adapter = {0};
    assert(clock_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _assert_psram_worker();
    _start_owned_sync_and_alarm(&adapter);
    assert(clock_demo_adapter_close(&adapter) == ESP_OK);
    _assert_worker_released(owner_delete_count);
    assert(!clock_demo_adapter_is_open(&adapter));

    (void)pthread_mutex_lock(&s_time.lock);
    assert(!s_time.sync_active);
    assert(!s_time.alarm_enabled);
    assert(s_time.sync_cancel_count == 1U);
    assert(s_time.alarm_disable_count == 1U);
    (void)pthread_mutex_unlock(&s_time.lock);
}

static void _test_failed_cleanup_retains_ownership_for_retry(void)
{
    _fake_time_reset();
    clock_demo_adapter_t adapter = {0};
    assert(clock_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _assert_psram_worker();
    _start_owned_sync_and_alarm(&adapter);

    (void)pthread_mutex_lock(&s_time.lock);
    s_time.next_disable_result = ESP_FAIL;
    s_time.next_cancel_result = ESP_ERR_TIMEOUT;
    (void)pthread_mutex_unlock(&s_time.lock);
    assert(clock_demo_adapter_close(&adapter) == ESP_FAIL);
    _assert_worker_retained(owner_delete_count);
    assert(clock_demo_adapter_is_open(&adapter));

    clock_demo_adapter_snapshot_t snapshot;
    assert(clock_demo_adapter_get_snapshot(&adapter, &snapshot) == ESP_OK);
    assert(snapshot.closing);
    assert(snapshot.alarm_owned);
    assert(snapshot.sync_owned);
    assert(snapshot.cleanup_result == ESP_FAIL);

    assert(clock_demo_adapter_close(&adapter) == ESP_OK);
    _assert_worker_released(owner_delete_count);
    assert(!clock_demo_adapter_is_open(&adapter));
    (void)pthread_mutex_lock(&s_time.lock);
    assert(!s_time.sync_active);
    assert(!s_time.alarm_enabled);
    assert(s_time.sync_cancel_count == 2U);
    assert(s_time.alarm_disable_count == 2U);
    (void)pthread_mutex_unlock(&s_time.lock);
}

time_service_quality_t time_service_get_quality(void)
{
    return TIME_SERVICE_QUALITY_RTC;
}

esp_err_t time_service_get_utc(struct tm *utc_time)
{
    if (utc_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t now = time(NULL);
    return gmtime_r(&now, utc_time) != NULL ? ESP_OK : ESP_FAIL;
}

esp_err_t time_service_request_sync(void)
{
    (void)pthread_mutex_lock(&s_time.lock);
    ++s_time.sync_request_count;
    s_time.sync_active = true;
    (void)pthread_mutex_unlock(&s_time.lock);
    return ESP_OK;
}

esp_err_t time_service_cancel_sync(void)
{
    (void)pthread_mutex_lock(&s_time.lock);
    ++s_time.sync_cancel_count;
    const esp_err_t result = s_time.next_cancel_result;
    s_time.next_cancel_result = ESP_OK;
    if (result == ESP_OK)
    {
        s_time.sync_active = false;
    }
    (void)pthread_mutex_unlock(&s_time.lock);
    return result;
}

esp_err_t time_service_alarm_get_status(time_service_alarm_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_time.lock);
    *status = (time_service_alarm_status_t)
    {
        .enabled = s_time.alarm_enabled,
    };
    (void)pthread_mutex_unlock(&s_time.lock);
    return ESP_OK;
}

esp_err_t time_service_alarm_configure(
    const time_service_alarm_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_time.lock);
    ++s_time.alarm_configure_count;
    s_time.last_alarm = *config;
    s_time.alarm_enabled = true;
    (void)pthread_mutex_unlock(&s_time.lock);
    return ESP_OK;
}

esp_err_t time_service_alarm_disable(void)
{
    (void)pthread_mutex_lock(&s_time.lock);
    ++s_time.alarm_disable_count;
    const esp_err_t result = s_time.next_disable_result;
    s_time.next_disable_result = ESP_OK;
    if (result == ESP_OK)
    {
        s_time.alarm_enabled = false;
    }
    (void)pthread_mutex_unlock(&s_time.lock);
    return result;
}

int main(void)
{
    _test_alarm_ten_seconds_from_now();
    _test_external_alarm_is_not_overwritten();
    _test_close_releases_owned_resources();
    _test_failed_cleanup_retains_ownership_for_retry();
    assert(pthread_mutex_destroy(&s_time.lock) == 0);
    puts("clock demo adapter tests passed");
    return 0;
}

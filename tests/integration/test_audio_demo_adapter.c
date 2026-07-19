#include "audio_demo_adapter.h"

#include "audio_service.h"
#include "esp_heap_caps.h"
#include "host_freertos.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_WAIT_ATTEMPTS       5000U
#define TEST_WAIT_INTERVAL_US    1000U
#define TEST_PCM_CHUNK_BYTES     640U
#define TEST_TONE_CHUNK_COUNT    100U
#define TEST_EVENT_CAPACITY      256U

typedef enum
{
    TEST_AUDIO_EVENT_START = 0,
    TEST_AUDIO_EVENT_STOP,
    TEST_AUDIO_EVENT_SET_VOLUME,
    TEST_AUDIO_EVENT_SET_MUTE,
    TEST_AUDIO_EVENT_PA_ON,
    TEST_AUDIO_EVENT_PA_OFF,
    TEST_AUDIO_EVENT_WRITE,
} test_audio_event_t;

typedef struct test_audio_fake_data
{
    audio_service_state_t state;
    pthread_t worker_thread;
    test_audio_event_t events[TEST_EVENT_CAPACITY];
    size_t event_count;
    size_t read_count;
    size_t read_with_pa_count;
    size_t write_count;
    size_t successful_write_count;
    size_t failed_write_count;
    size_t write_without_pa_count;
    size_t writes_since_read;
    size_t maximum_writes_without_read;
    size_t smallest_read_with_pa;
    size_t largest_read_with_pa;
    uint32_t maximum_read_timeout_with_pa_ms;
    size_t service_call_count;
    size_t smallest_write;
    size_t largest_write;
    unsigned active_calls;
    unsigned maximum_active_calls;
    unsigned pa_enable_count;
    unsigned pa_disable_count;
    unsigned next_stop_delay_us;
    int16_t read_peak;
    uint8_t volume;
    bool muted;
    bool pa_enabled;
    bool worker_thread_set;
    bool worker_thread_mismatch;
    bool fail_next_volume;
    bool fail_next_mute;
    bool fail_next_write;
    bool fail_next_stop;
} test_audio_fake_data_t;

typedef struct test_audio_fake
{
    pthread_mutex_t lock;
    test_audio_fake_data_t data;
} test_audio_fake_t;

static test_audio_fake_t s_audio_fake =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _test_assert_no_invalid_worker_delete(void)
{
    assert(host_caps_task_wrong_delete_count() == 0U);
    assert(host_caps_task_self_delete_count() == 0U);
}

static size_t _test_assert_psram_worker(void)
{
    const UBaseType_t required = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    assert(host_caps_task_count() == 1U);
    assert((host_last_task_stack_caps() & required) == required);
    _test_assert_no_invalid_worker_delete();
    return host_caps_task_owner_delete_count();
}

static void _test_assert_worker_retained(size_t owner_delete_count)
{
    assert(host_caps_task_count() == 1U);
    assert(host_caps_task_owner_delete_count() == owner_delete_count);
    _test_assert_no_invalid_worker_delete();
}

static void _test_assert_worker_released(size_t owner_delete_count)
{
    assert(host_caps_task_count() == 0U);
    assert(host_caps_task_owner_delete_count() == owner_delete_count + 1U);
    _test_assert_no_invalid_worker_delete();
}

static void _test_audio_fake_reset(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    memset(&s_audio_fake.data, 0, sizeof(s_audio_fake.data));
    s_audio_fake.data.state = AUDIO_SERVICE_STATE_READY;
    s_audio_fake.data.volume = 60U;
    s_audio_fake.data.smallest_write = SIZE_MAX;
    s_audio_fake.data.smallest_read_with_pa = SIZE_MAX;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_fake_enter(bool record, test_audio_event_t event)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const pthread_t current = pthread_self();
    if (!s_audio_fake.data.worker_thread_set)
    {
        s_audio_fake.data.worker_thread = current;
        s_audio_fake.data.worker_thread_set = true;
    }
    else if (!pthread_equal(s_audio_fake.data.worker_thread, current))
    {
        s_audio_fake.data.worker_thread_mismatch = true;
    }
    ++s_audio_fake.data.service_call_count;
    ++s_audio_fake.data.active_calls;
    if (s_audio_fake.data.active_calls >
            s_audio_fake.data.maximum_active_calls)
    {
        s_audio_fake.data.maximum_active_calls =
            s_audio_fake.data.active_calls;
    }
    if (record && s_audio_fake.data.event_count < TEST_EVENT_CAPACITY)
    {
        s_audio_fake.data.events[s_audio_fake.data.event_count++] = event;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_fake_leave(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    assert(s_audio_fake.data.active_calls > 0U);
    --s_audio_fake.data.active_calls;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static test_audio_fake_data_t _test_audio_fake_snapshot(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const test_audio_fake_data_t snapshot = s_audio_fake.data;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    return snapshot;
}

static void _test_audio_fail_next_write(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.fail_next_write = true;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_fail_next_volume(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.fail_next_volume = true;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_fail_next_mute(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.fail_next_mute = true;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_begin_worker_session(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.worker_thread_set = false;
    s_audio_fake.data.worker_thread_mismatch = false;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_fail_next_stop(void)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.fail_next_stop = true;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_delay_next_stop(unsigned delay_us)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.next_stop_delay_us = delay_us;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static void _test_audio_set_read_peak(int16_t peak)
{
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.read_peak = peak;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
}

static bool _test_wait_for_adapter_state(audio_demo_adapter_t *adapter,
        audio_demo_adapter_state_t expected)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        audio_demo_snapshot_t snapshot;
        if (audio_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.state == expected)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_tone(audio_demo_adapter_t *adapter,
                                audio_demo_tone_state_t expected)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        audio_demo_snapshot_t snapshot;
        if (audio_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.tone_state == expected)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_controls(audio_demo_adapter_t *adapter,
                                    uint8_t volume, bool muted)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        audio_demo_snapshot_t snapshot;
        if (audio_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.volume_percent == volume && snapshot.muted == muted)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_volume_request(audio_demo_adapter_t *adapter,
        uint32_t request_id, esp_err_t result, uint8_t volume)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        audio_demo_snapshot_t snapshot;
        if (audio_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.volume_request_id == request_id &&
                snapshot.volume_result == result &&
                snapshot.volume_percent == volume)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_mute_request(audio_demo_adapter_t *adapter,
                                        uint32_t request_id,
                                        esp_err_t result, bool muted)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        audio_demo_snapshot_t snapshot;
        if (audio_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.mute_request_id == request_id &&
                snapshot.mute_result == result && snapshot.muted == muted)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_read_count(size_t expected)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        if (_test_audio_fake_snapshot().read_count >= expected)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_mic_level(audio_demo_adapter_t *adapter,
                                     uint8_t minimum, uint8_t maximum)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        audio_demo_snapshot_t snapshot;
        if (audio_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.mic_level_percent >= minimum &&
                snapshot.mic_level_percent <= maximum)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static bool _test_wait_for_write_count(size_t expected)
{
    for (unsigned attempt = 0U; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        if (_test_audio_fake_snapshot().write_count >= expected)
        {
            return true;
        }
        (void)usleep(TEST_WAIT_INTERVAL_US);
    }
    return false;
}

static size_t _test_find_event(const test_audio_fake_data_t *snapshot,
                               test_audio_event_t event, size_t start)
{
    for (size_t index = start; index < snapshot->event_count; ++index)
    {
        if (snapshot->events[index] == event)
        {
            return index;
        }
    }
    return SIZE_MAX;
}

audio_service_config_t audio_service_get_default_config(void)
{
    _test_audio_fake_enter(false, TEST_AUDIO_EVENT_START);
    const audio_service_config_t config =
    {
        .sample_rate_hz = 16000U,
        .bits_per_sample = 16U,
        .channels = 2U,
        .mclk_multiple = 384U,
    };
    _test_audio_fake_leave();
    return config;
}

bool audio_service_is_available(void)
{
    _test_audio_fake_enter(false, TEST_AUDIO_EVENT_START);
    _test_audio_fake_leave();
    return true;
}

audio_service_state_t audio_service_get_state(void)
{
    _test_audio_fake_enter(false, TEST_AUDIO_EVENT_START);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const audio_service_state_t state = s_audio_fake.data.state;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return state;
}

esp_err_t audio_service_start(void)
{
    _test_audio_fake_enter(true, TEST_AUDIO_EVENT_START);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const bool valid = s_audio_fake.data.state == AUDIO_SERVICE_STATE_READY ||
                       s_audio_fake.data.state == AUDIO_SERVICE_STATE_RUNNING;
    if (valid)
    {
        s_audio_fake.data.state = AUDIO_SERVICE_STATE_RUNNING;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_service_stop(void)
{
    _test_audio_fake_enter(true, TEST_AUDIO_EVENT_STOP);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const bool fail = s_audio_fake.data.fail_next_stop;
    const unsigned delay_us = s_audio_fake.data.next_stop_delay_us;
    s_audio_fake.data.fail_next_stop = false;
    s_audio_fake.data.next_stop_delay_us = 0U;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    if (delay_us != 0U)
    {
        (void)usleep(delay_us);
    }
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    if (!fail)
    {
        s_audio_fake.data.state = AUDIO_SERVICE_STATE_READY;
        s_audio_fake.data.pa_enabled = false;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return fail ? ESP_FAIL : ESP_OK;
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

    _test_audio_fake_enter(true, TEST_AUDIO_EVENT_WRITE);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const bool running =
        s_audio_fake.data.state == AUDIO_SERVICE_STATE_RUNNING;
    const bool pa_enabled = s_audio_fake.data.pa_enabled;
    const bool fail = s_audio_fake.data.fail_next_write;
    s_audio_fake.data.fail_next_write = false;
    ++s_audio_fake.data.write_count;
    ++s_audio_fake.data.writes_since_read;
    if (s_audio_fake.data.writes_since_read >
            s_audio_fake.data.maximum_writes_without_read)
    {
        s_audio_fake.data.maximum_writes_without_read =
            s_audio_fake.data.writes_since_read;
    }
    if (!pa_enabled)
    {
        ++s_audio_fake.data.write_without_pa_count;
    }
    if (bytes < s_audio_fake.data.smallest_write)
    {
        s_audio_fake.data.smallest_write = bytes;
    }
    if (bytes > s_audio_fake.data.largest_write)
    {
        s_audio_fake.data.largest_write = bytes;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);

    (void)usleep(2000U);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    if (fail)
    {
        ++s_audio_fake.data.failed_write_count;
    }
    else if (running)
    {
        ++s_audio_fake.data.successful_write_count;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    if (!fail && running && written != NULL)
    {
        *written = bytes;
    }
    _test_audio_fake_leave();
    return !running ? ESP_ERR_INVALID_STATE : (fail ? ESP_FAIL : ESP_OK);
}

esp_err_t audio_service_read(void *data, size_t bytes, size_t *read,
                             uint32_t timeout_ms)
{
    if (read != NULL)
    {
        *read = 0U;
    }
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    _test_audio_fake_enter(false, TEST_AUDIO_EVENT_START);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const bool running =
        s_audio_fake.data.state == AUDIO_SERVICE_STATE_RUNNING;
    const int16_t peak = s_audio_fake.data.read_peak;
    ++s_audio_fake.data.read_count;
    s_audio_fake.data.writes_since_read = 0U;
    if (s_audio_fake.data.pa_enabled)
    {
        ++s_audio_fake.data.read_with_pa_count;
        if (bytes < s_audio_fake.data.smallest_read_with_pa)
        {
            s_audio_fake.data.smallest_read_with_pa = bytes;
        }
        if (bytes > s_audio_fake.data.largest_read_with_pa)
        {
            s_audio_fake.data.largest_read_with_pa = bytes;
        }
        if (timeout_ms > s_audio_fake.data.maximum_read_timeout_with_pa_ms)
        {
            s_audio_fake.data.maximum_read_timeout_with_pa_ms = timeout_ms;
        }
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    (void)usleep(2000U);
    if (running)
    {
        memset(data, 0, bytes);
        if (bytes >= sizeof(peak))
        {
            memcpy(data, &peak, sizeof(peak));
        }
        if (read != NULL)
        {
            *read = bytes;
        }
    }
    _test_audio_fake_leave();
    return running ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_service_set_volume(uint8_t percent)
{
    if (percent > 100U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _test_audio_fake_enter(true, TEST_AUDIO_EVENT_SET_VOLUME);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const bool fail = s_audio_fake.data.fail_next_volume;
    s_audio_fake.data.fail_next_volume = false;
    if (!fail)
    {
        s_audio_fake.data.volume = percent;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return fail ? ESP_FAIL : ESP_OK;
}

esp_err_t audio_service_get_volume(uint8_t *percent)
{
    if (percent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _test_audio_fake_enter(false, TEST_AUDIO_EVENT_START);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    *percent = s_audio_fake.data.volume;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return ESP_OK;
}

esp_err_t audio_service_set_mute(bool muted)
{
    _test_audio_fake_enter(true, TEST_AUDIO_EVENT_SET_MUTE);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    const bool fail = s_audio_fake.data.fail_next_mute;
    s_audio_fake.data.fail_next_mute = false;
    if (!fail)
    {
        s_audio_fake.data.muted = muted;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return fail ? ESP_FAIL : ESP_OK;
}

esp_err_t audio_service_get_mute(bool *muted)
{
    if (muted == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _test_audio_fake_enter(false, TEST_AUDIO_EVENT_START);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    *muted = s_audio_fake.data.muted;
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return ESP_OK;
}

esp_err_t audio_service_set_pa(bool enabled)
{
    _test_audio_fake_enter(true, enabled ? TEST_AUDIO_EVENT_PA_ON :
                           TEST_AUDIO_EVENT_PA_OFF);
    (void)pthread_mutex_lock(&s_audio_fake.lock);
    s_audio_fake.data.pa_enabled = enabled;
    if (enabled)
    {
        ++s_audio_fake.data.pa_enable_count;
    }
    else
    {
        ++s_audio_fake.data.pa_disable_count;
    }
    (void)pthread_mutex_unlock(&s_audio_fake.lock);
    _test_audio_fake_leave();
    return ESP_OK;
}

static void _test_low_level_pcm_drives_meter(void)
{
    _test_audio_fake_reset();
    _test_audio_set_read_peak(-64);
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));
    assert(_test_wait_for_mic_level(&adapter, 11U, 11U));

    _test_audio_set_read_peak(0);
    assert(_test_wait_for_mic_level(&adapter, 0U, 0U));
    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
}

static void _test_command_serialization(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));

    uint32_t request_id = 0U;
    assert(audio_demo_adapter_set_volume(&adapter, 37U, &request_id) ==
           ESP_OK);
    assert(request_id != 0U);
    uint32_t mute_request_id = 0U;
    assert(audio_demo_adapter_set_mute(&adapter, true,
                                       &mute_request_id) == ESP_OK);
    assert(mute_request_id != 0U);
    assert(_test_wait_for_controls(&adapter, 37U, true));
    assert(_test_wait_for_volume_request(&adapter, request_id, ESP_OK, 37U));
    assert(_test_wait_for_mute_request(&adapter, mute_request_id,
                                       ESP_OK, true));
    assert(_test_wait_for_read_count(3U));
    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);

    const test_audio_fake_data_t fake = _test_audio_fake_snapshot();
    const size_t volume_index = _test_find_event(
                                    &fake, TEST_AUDIO_EVENT_SET_VOLUME, 0U);
    const size_t mute_index = _test_find_event(
                                  &fake, TEST_AUDIO_EVENT_SET_MUTE, 0U);
    assert(volume_index != SIZE_MAX);
    assert(mute_index != SIZE_MAX);
    assert(volume_index < mute_index);
    assert(fake.maximum_active_calls == 1U);
    assert(!fake.worker_thread_mismatch);
    assert(fake.volume == 37U);
    assert(fake.muted);
}

static void _test_volume_request_completion_and_recovery(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));

    uint32_t first_request_id = 0U;
    assert(audio_demo_adapter_set_volume(&adapter, 37U,
                                         &first_request_id) == ESP_OK);
    assert(first_request_id != 0U);
    assert(_test_wait_for_volume_request(&adapter, first_request_id,
                                         ESP_OK, 37U));

    _test_audio_fail_next_volume();
    uint32_t failed_request_id = 0U;
    assert(audio_demo_adapter_set_volume(&adapter, 82U,
                                         &failed_request_id) == ESP_OK);
    assert(failed_request_id != 0U);
    assert(failed_request_id != first_request_id);
    assert(_test_wait_for_volume_request(&adapter, failed_request_id,
                                         ESP_FAIL, 37U));
    const size_t reads_after_failure =
        _test_audio_fake_snapshot().read_count;
    assert(_test_wait_for_read_count(reads_after_failure + 2U));
    assert(_test_wait_for_volume_request(&adapter, failed_request_id,
                                         ESP_FAIL, 37U));

    uint32_t recovery_request_id = 0U;
    assert(audio_demo_adapter_set_volume(&adapter, 55U,
                                         &recovery_request_id) == ESP_OK);
    assert(recovery_request_id != 0U);
    assert(recovery_request_id != failed_request_id);
    assert(_test_wait_for_volume_request(&adapter, recovery_request_id,
                                         ESP_OK, 55U));

    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
}

static void _test_mute_request_completion_and_recovery(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));

    uint32_t first_request_id = 0U;
    assert(audio_demo_adapter_set_mute(&adapter, true,
                                       &first_request_id) == ESP_OK);
    assert(first_request_id != 0U);
    assert(_test_wait_for_mute_request(&adapter, first_request_id,
                                       ESP_OK, true));

    _test_audio_fail_next_mute();
    uint32_t failed_request_id = 0U;
    assert(audio_demo_adapter_set_mute(&adapter, false,
                                       &failed_request_id) == ESP_OK);
    assert(failed_request_id != 0U);
    assert(failed_request_id != first_request_id);
    assert(_test_wait_for_mute_request(&adapter, failed_request_id,
                                       ESP_FAIL, true));
    const size_t reads_after_failure =
        _test_audio_fake_snapshot().read_count;
    assert(_test_wait_for_read_count(reads_after_failure + 2U));
    assert(_test_wait_for_mute_request(&adapter, failed_request_id,
                                       ESP_FAIL, true));

    uint32_t recovery_request_id = 0U;
    assert(audio_demo_adapter_set_mute(&adapter, false,
                                       &recovery_request_id) == ESP_OK);
    assert(recovery_request_id != 0U);
    assert(recovery_request_id != failed_request_id);
    assert(_test_wait_for_mute_request(&adapter, recovery_request_id,
                                       ESP_OK, false));

    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
}

static void _test_open_close_open_restarts_capture(void)
{
    _test_audio_fake_reset();
    _test_audio_set_read_peak(-64);
    audio_demo_adapter_t adapter = {0};

    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));
    assert(_test_wait_for_mic_level(&adapter, 11U, 11U));
    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);

    const size_t first_session_reads =
        _test_audio_fake_snapshot().read_count;
    _test_audio_begin_worker_session();
    _test_audio_set_read_peak(-512);
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));
    assert(_test_wait_for_read_count(first_session_reads + 1U));
    assert(_test_wait_for_mic_level(&adapter, 44U, 44U));
    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);

    const test_audio_fake_data_t fake = _test_audio_fake_snapshot();
    assert(fake.state == AUDIO_SERVICE_STATE_READY);
    assert(!fake.worker_thread_mismatch);
    assert(!fake.pa_enabled);
}

static void _test_chunked_tone_cancel_and_pa_window(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));
    assert(_test_wait_for_read_count(4U));

    test_audio_fake_data_t fake = _test_audio_fake_snapshot();
    assert(!fake.pa_enabled);
    assert(fake.pa_enable_count == 0U);
    assert(fake.read_with_pa_count == 0U);

    assert(audio_demo_adapter_play_tone(&adapter) == ESP_OK);
    assert(_test_wait_for_tone(&adapter, AUDIO_DEMO_TONE_PLAYING));
    assert(_test_wait_for_write_count(3U));
    fake = _test_audio_fake_snapshot();
    assert(fake.pa_enabled);
    assert(audio_demo_adapter_cancel_tone(&adapter) == ESP_OK);
    assert(_test_wait_for_tone(&adapter, AUDIO_DEMO_TONE_CANCELLED));
    const size_t reads_before_resume = fake.read_count;
    assert(_test_wait_for_read_count(reads_before_resume + 2U));

    fake = _test_audio_fake_snapshot();
    assert(fake.write_count >= 3U);
    assert(fake.write_count < TEST_TONE_CHUNK_COUNT);
    assert(fake.smallest_write == TEST_PCM_CHUNK_BYTES);
    assert(fake.largest_write == TEST_PCM_CHUNK_BYTES);
    assert(fake.write_without_pa_count == 0U);
    assert(fake.read_with_pa_count > 0U);
    assert(fake.maximum_writes_without_read == 1U);
    assert(fake.smallest_read_with_pa == TEST_PCM_CHUNK_BYTES);
    assert(fake.largest_read_with_pa == TEST_PCM_CHUNK_BYTES);
    assert(fake.maximum_read_timeout_with_pa_ms == 0U);
    assert(fake.pa_enable_count == 1U);
    assert(fake.pa_disable_count >= 2U);
    assert(!fake.pa_enabled);
    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
}

static void _test_write_error_recovers_on_next_tone(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));

    _test_audio_fail_next_write();
    assert(audio_demo_adapter_play_tone(&adapter) == ESP_OK);
    assert(_test_wait_for_tone(&adapter, AUDIO_DEMO_TONE_ERROR));
    test_audio_fake_data_t fake = _test_audio_fake_snapshot();
    assert(fake.failed_write_count == 1U);
    assert(fake.write_count == 1U);
    assert(!fake.pa_enabled);
    const size_t reads_after_write_error = fake.read_count;
    assert(_test_wait_for_read_count(reads_after_write_error + 1U));

    assert(audio_demo_adapter_play_tone(&adapter) == ESP_OK);
    assert(_test_wait_for_tone(&adapter, AUDIO_DEMO_TONE_COMPLETE));
    fake = _test_audio_fake_snapshot();
    assert(fake.failed_write_count == 1U);
    assert(fake.successful_write_count == TEST_TONE_CHUNK_COUNT);
    assert(fake.write_count == TEST_TONE_CHUNK_COUNT + 1U);
    assert(fake.write_without_pa_count == 0U);
    assert(fake.read_with_pa_count == TEST_TONE_CHUNK_COUNT);
    assert(fake.maximum_writes_without_read == 1U);
    assert(fake.smallest_read_with_pa == TEST_PCM_CHUNK_BYTES);
    assert(fake.largest_read_with_pa == TEST_PCM_CHUNK_BYTES);
    assert(fake.maximum_read_timeout_with_pa_ms == 0U);
    assert(fake.pa_enable_count == 2U);
    assert(!fake.pa_enabled);
    const size_t reads_at_completion = fake.read_count;
    _test_audio_set_read_peak(-512);
    assert(_test_wait_for_read_count(reads_at_completion + 1U));
    assert(_test_wait_for_mic_level(&adapter, 44U, 44U));
    const size_t reads_after_first_level =
        _test_audio_fake_snapshot().read_count;
    _test_audio_set_read_peak(-2048);
    assert(_test_wait_for_read_count(reads_after_first_level + 1U));
    assert(_test_wait_for_mic_level(&adapter, 66U, 66U));
    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
}

static void _test_failed_close_keeps_commands_rejected(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));

    _test_audio_fail_next_stop();
    assert(audio_demo_adapter_close(&adapter) == ESP_FAIL);
    _test_assert_worker_retained(owner_delete_count);
    assert(audio_demo_adapter_is_open(&adapter));

    audio_demo_snapshot_t snapshot;
    assert(audio_demo_adapter_get_snapshot(&adapter, &snapshot) == ESP_OK);
    assert(snapshot.state == AUDIO_DEMO_ADAPTER_ERROR);
    assert(snapshot.last_error == ESP_FAIL);

    const test_audio_fake_data_t before = _test_audio_fake_snapshot();
    assert(before.state == AUDIO_SERVICE_STATE_RUNNING);
    assert(audio_demo_adapter_play_tone(&adapter) == ESP_ERR_INVALID_STATE);
    assert(audio_demo_adapter_cancel_tone(&adapter) == ESP_ERR_INVALID_STATE);
    uint32_t request_id = UINT32_MAX;
    assert(audio_demo_adapter_set_volume(&adapter, 23U, &request_id) ==
           ESP_ERR_INVALID_STATE);
    assert(request_id == UINT32_MAX);
    uint32_t mute_request_id = UINT32_MAX;
    assert(audio_demo_adapter_set_mute(&adapter, true, &mute_request_id) ==
           ESP_ERR_INVALID_STATE);
    assert(mute_request_id == UINT32_MAX);
    const test_audio_fake_data_t after = _test_audio_fake_snapshot();
    assert(after.service_call_count == before.service_call_count);
    assert(after.event_count == before.event_count);
    assert(after.volume == before.volume);
    assert(after.muted == before.muted);

    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
    assert(!audio_demo_adapter_is_open(&adapter));
    const test_audio_fake_data_t closed = _test_audio_fake_snapshot();
    assert(closed.service_call_count == before.service_call_count + 1U);
    assert(closed.event_count == before.event_count + 1U);
    assert(closed.events[closed.event_count - 1U] == TEST_AUDIO_EVENT_STOP);
    assert(closed.state == AUDIO_SERVICE_STATE_READY);
    assert(!closed.pa_enabled);
}

static void _test_late_close_completion_does_not_poison_retry(void)
{
    _test_audio_fake_reset();
    audio_demo_adapter_t adapter = {0};
    assert(audio_demo_adapter_open(&adapter) == ESP_OK);
    const size_t owner_delete_count = _test_assert_psram_worker();
    assert(_test_wait_for_adapter_state(&adapter,
                                        AUDIO_DEMO_ADAPTER_RUNNING));

    _test_audio_fail_next_stop();
    _test_audio_delay_next_stop(550000U);
    assert(audio_demo_adapter_close(&adapter) == ESP_ERR_TIMEOUT);
    _test_assert_worker_retained(owner_delete_count);

    assert(audio_demo_adapter_close(&adapter) == ESP_OK);
    _test_assert_worker_released(owner_delete_count);
    assert(!audio_demo_adapter_is_open(&adapter));

    const test_audio_fake_data_t closed = _test_audio_fake_snapshot();
    assert(closed.state == AUDIO_SERVICE_STATE_READY);
    assert(closed.event_count >= 2U);
    assert(closed.events[closed.event_count - 2U] == TEST_AUDIO_EVENT_STOP);
    assert(closed.events[closed.event_count - 1U] == TEST_AUDIO_EVENT_STOP);
}

int main(void)
{
    _test_low_level_pcm_drives_meter();
    _test_command_serialization();
    _test_volume_request_completion_and_recovery();
    _test_mute_request_completion_and_recovery();
    _test_open_close_open_restarts_capture();
    _test_chunked_tone_cancel_and_pa_window();
    _test_write_error_recovers_on_next_tone();
    _test_failed_close_keeps_commands_rejected();
    _test_late_close_completion_does_not_poison_retry();
    puts("audio demo adapter tests passed");
    return 0;
}

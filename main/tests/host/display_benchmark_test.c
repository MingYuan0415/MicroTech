#include "display_benchmark.h"

#include "app_manager.h"
#include "app_manager_display_diagnostics.h"
#include "audio_service.h"
#include "display_benchmark_host_port.h"
#include "esp_heap_caps.h"
#include "wifi_service.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if TEST_DISPLAY_BENCHMARK_MODE_STRESS || \
    TEST_DISPLAY_BENCHMARK_LOAD_FULL
    #define TEST_REQUIRES_AUDIO 1
    #define TEST_REQUIRES_TCP   1
    #define TEST_LOAD_PROFILE   "full"
#elif TEST_DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY
    #define TEST_REQUIRES_AUDIO 1
    #define TEST_REQUIRES_TCP   0
    #define TEST_LOAD_PROFILE   "audio-only"
#else
    #define TEST_REQUIRES_AUDIO 0
    #define TEST_REQUIRES_TCP   1
    #define TEST_LOAD_PROFILE   "tcp-only"
#endif

#if CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION
    #define TEST_SNAPSHOT_NAME "enabled"
    #define TEST_SNAPSHOT_PREPARE_COUNT_TEXT "snapshot_prepare_count=2"
    #define TEST_SNAPSHOT_PREPARE_US_TEXT "snapshot_prepare_us=80000"
#else
    #define TEST_SNAPSHOT_NAME "n/a"
    #define TEST_SNAPSHOT_PREPARE_COUNT_TEXT "snapshot_prepare_count=0"
    #define TEST_SNAPSHOT_PREPARE_US_TEXT "snapshot_prepare_us=0"
#endif

typedef struct echo_server
{
    pthread_t thread;
    atomic_int listener;
    atomic_int connection;
    atomic_bool corrupt;
    atomic_bool reset_first_connection;
    atomic_uint first_response_delay_us;
    atomic_uint connection_count;
} echo_server_t;

#if TEST_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
#define TEST_BENCHMARK_TOTAL_US \
        (DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US * 10LL)
#define TEST_TCP_ACTIVE_US \
        (DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US * 5LL)
#define TEST_EXPECTED_FRAME_SUBMITS 20U
#define TEST_EXPECTED_LOCK_ERRORS   2U
#else
#define TEST_BENCHMARK_TOTAL_US DISPLAY_BENCHMARK_DURATION_US
#define TEST_TCP_ACTIVE_US      DISPLAY_BENCHMARK_DURATION_US
#define TEST_EXPECTED_FRAME_SUBMITS 10U
#define TEST_EXPECTED_LOCK_ERRORS   1U
#endif

static const display_benchmark_config_t s_benchmark_config =
{
#if TEST_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
    .mode = DISPLAY_BENCHMARK_MODE_CHARACTERIZATION,
#else
    .mode = DISPLAY_BENCHMARK_MODE_STRESS,
#endif
    .stress_duration_sec = 1800U,
    .effect_duration_sec = 30U,
#if TEST_DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY
    .load = DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY,
#elif TEST_DISPLAY_BENCHMARK_LOAD_TCP_ONLY
    .load = DISPLAY_BENCHMARK_LOAD_TCP_ONLY,
#else
    .load = DISPLAY_BENCHMARK_LOAD_FULL,
#endif
    .ipv4_host = "127.0.0.1",
    .port = TEST_DISPLAY_BENCHMARK_TCP_PORT,
    .rate_kbit_s = TEST_DISPLAY_BENCHMARK_TCP_RATE_KBIT_S,
};

#define display_benchmark_start() display_benchmark_start(&s_benchmark_config)

#define TEST_TCP_PAYLOAD_BYTES \
    ((CONFIG_LWIP_TCP_SND_BUF_DEFAULT / CONFIG_LWIP_TCP_MSS) * \
     CONFIG_LWIP_TCP_MSS)

_Static_assert(TEST_TCP_PAYLOAD_BYTES == 5760U,
               "host benchmark must exercise the target TCP payload");
_Static_assert(TEST_TCP_PAYLOAD_BYTES <= CONFIG_LWIP_TCP_SND_BUF_DEFAULT,
               "host TCP payload exceeds the send buffer");
_Static_assert(TEST_TCP_PAYLOAD_BYTES + CONFIG_LWIP_TCP_MSS >
               CONFIG_LWIP_TCP_SND_BUF_DEFAULT,
               "host TCP payload is not the largest whole MSS multiple");

typedef enum test_report_quality
{
    TEST_REPORT_TARGET = 0,
    TEST_REPORT_FLOOR,
    TEST_REPORT_FAIL,
    TEST_REPORT_STABILITY_FAIL,
    TEST_REPORT_SNAPSHOT_PREPARE_SLOW,
    TEST_REPORT_SNAPSHOT_FALLBACK,
    TEST_REPORT_SNAPSHOT_PREPARE_SLOW_AUXILIARY,
    TEST_REPORT_SNAPSHOT_FALLBACK_AUXILIARY,
    TEST_REPORT_SNAPSHOT_FALLBACK_WITHOUT_START,
} test_report_quality_t;

EVENT_BUS_DEFINE_ID(WIFI_SERVICE_MSG);

static pthread_mutex_t s_event_bus_lock = PTHREAD_MUTEX_INITIALIZER;
static event_bus_cb_t s_wifi_event_callback;
static void *s_wifi_event_user_data;
static atomic_bool s_event_unsubscribe_fail_once;
static atomic_uint s_event_subscribe_count;
static atomic_uint s_event_unsubscribe_count;
static atomic_int s_wifi_state;
static atomic_bool s_audio_fail;
static atomic_uint s_audio_start_count;
static atomic_uint s_audio_stop_count;
static atomic_uint s_audio_write_count;
static atomic_uint s_audio_read_count;
static atomic_uint s_diagnostics_begin_count;
static atomic_uint s_diagnostics_end_count;
static atomic_uint s_navigation_count;
static atomic_uint s_presentation_wait_count;
static atomic_int s_presentation_wait_result;
static atomic_uint s_activity_count;
static atomic_bool s_saw_fade;
static atomic_bool s_saw_push_left;
static atomic_bool s_saw_push_right;
static atomic_bool s_saw_cover_left;
static atomic_bool s_saw_reveal_right;
static atomic_uint s_profile_effect_mask;
static atomic_int s_report_quality;
static atomic_uint s_log_result;
static atomic_uint s_log_performance;
static atomic_uint s_log_effect_count;
static atomic_uint s_log_sample_error_count;
static atomic_uint s_log_lvgl_lock_error_count;
static atomic_uint s_log_fps_read_error_count;
static atomic_uint s_log_maximum_fps_lock_wait_us;
static atomic_uint s_log_frame_submit_count;
static atomic_bool s_log_has_legacy_fields;
static atomic_bool s_log_config_valid;
static atomic_bool s_log_load_valid;
static atomic_uint s_log_second_profile_effect_count;
static atomic_uint s_log_tcp_required;
static atomic_uint s_log_control_error;
static atomic_uint s_log_audio_error;
static atomic_uint s_log_tcp_error;
static atomic_uint_fast64_t s_log_tcp_transmit_bytes;
static atomic_uint_fast64_t s_log_tcp_receive_bytes;
static atomic_uint_fast64_t s_log_tcp_target_bytes;
static atomic_uint_fast64_t s_log_tcp_active_us;
static atomic_uint_fast64_t s_log_tcp_down_ms;
static atomic_uint_fast64_t s_log_tcp_maximum_pacing_lag_us;
static atomic_uint s_log_tcp_rate_ok;
static atomic_uint s_log_tcp_reconnect_count;
static atomic_uint s_log_tcp_pacing_late_count;
static atomic_uint s_log_wifi_disconnect_count;
static atomic_uint s_heap_allocate_count;
static atomic_uint s_heap_free_count;
static atomic_size_t s_heap_maximum_allocation;
static uint32_t s_minimum_fps;
static size_t s_minimum_dma_largest;

static int64_t _monotonic_us(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000LL;
}

int64_t esp_timer_get_time(void)
{
    return _monotonic_us();
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    atomic_fetch_add(&s_heap_allocate_count, 1U);
    size_t observed = atomic_load(&s_heap_maximum_allocation);
    while (size > observed &&
            !atomic_compare_exchange_weak(&s_heap_maximum_allocation,
                                          &observed, size))
    {
    }
    return malloc(size);
}

void heap_caps_free(void *memory)
{
    if (memory != NULL)
    {
        atomic_fetch_add(&s_heap_free_count, 1U);
    }
    free(memory);
}

void test_log_write(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    const char *final_report = strstr(message, "display benchmark stability=");
    if (final_report != NULL)
    {
        char stability[8];
        char performance[8];
        assert(sscanf(final_report,
                      "display benchmark stability=%7s performance=%7s",
                      stability, performance) == 2);
        atomic_store(&s_log_result,
                     strcmp(stability, "PASS") == 0 ? 1U : 2U);
        if (strcmp(performance, "TARGET") == 0)
        {
            atomic_store(&s_log_performance, 1U);
        }
        else if (strcmp(performance, "FLOOR") == 0)
        {
            atomic_store(&s_log_performance, 2U);
        }
        else
        {
            assert(strcmp(performance, "FAIL") == 0);
            atomic_store(&s_log_performance, 3U);
        }
    }
    if (strstr(message, "display perf load=") != NULL)
    {
        assert(strstr(message, "snapshot=" TEST_SNAPSHOT_NAME) != NULL);
        assert(strstr(message, TEST_SNAPSHOT_PREPARE_COUNT_TEXT) != NULL);
        assert(strstr(message, TEST_SNAPSHOT_PREPARE_US_TEXT) != NULL);
        atomic_fetch_add(&s_log_effect_count, 1U);
        char expected_load[32];
        (void)snprintf(expected_load, sizeof(expected_load),
                       "display perf load=%s ", TEST_LOAD_PROFILE);
        if (strstr(message, expected_load) != NULL)
        {
            atomic_fetch_add(&s_log_second_profile_effect_count, 1U);
        }
    }
    const char *sample_error = strstr(message, " sample_err=");
    if (sample_error != NULL)
    {
        unsigned sample_error_count = 0U;
        assert(sscanf(sample_error, " sample_err=%u", &sample_error_count) ==
               1);
        atomic_store(&s_log_sample_error_count, sample_error_count);
    }
    const char *lock_error = strstr(message, " lock_err=");
    if (lock_error != NULL)
    {
        unsigned lock_error_count = 0U;
        assert(sscanf(lock_error, " lock_err=%u", &lock_error_count) == 1);
        atomic_store(&s_log_lvgl_lock_error_count, lock_error_count);
    }
    const char *fps_read_error = strstr(message, " fps_read_err=");
    if (fps_read_error != NULL)
    {
        unsigned fps_read_error_count = 0U;
        assert(sscanf(fps_read_error, " fps_read_err=%u",
                      &fps_read_error_count) == 1);
        atomic_store(&s_log_fps_read_error_count, fps_read_error_count);
    }
    const char *fps_lock_wait = strstr(message, " fps_lock_max_us=");
    if (fps_lock_wait != NULL)
    {
        unsigned maximum_fps_lock_wait_us = 0U;
        assert(sscanf(fps_lock_wait, " fps_lock_max_us=%u",
                      &maximum_fps_lock_wait_us) == 1);
        atomic_store(&s_log_maximum_fps_lock_wait_us,
                     maximum_fps_lock_wait_us);
    }
    const char *submits = strstr(message, " frame_submits=");
    if (submits != NULL)
    {
        unsigned frame_submit_count = 0U;
        assert(sscanf(submits, " frame_submits=%u", &frame_submit_count) == 1);
        atomic_store(&s_log_frame_submit_count, frame_submit_count);
    }
    if (strstr(message, " frames=") != NULL ||
            strstr(message, " fence_fail=") != NULL)
    {
        atomic_store(&s_log_has_legacy_fields, true);
    }
    if (strstr(message, "display config ") != NULL)
    {
        assert(strstr(message, "qspi_hz=40000000") != NULL);
        assert(strstr(message, "draw_rows=60") != NULL);
        assert(strstr(message, "color=RGB565") != NULL);
        assert(strstr(message, "snapshot=" TEST_SNAPSHOT_NAME) != NULL);
        assert(strstr(message, "dma_rows=10") != NULL);
        assert(strstr(message, "dma_max_full_rows=44") != NULL);
        assert(strstr(message, "queue=2") != NULL);
        assert(strstr(message, "direct=0") != NULL);
        assert(strstr(message, "te=0") != NULL);
        assert(strstr(message, "draw_units=2") != NULL);
        assert(strstr(message, "draw_prio=3") != NULL);
        assert(strstr(message, "tcp_payload=5760") != NULL);
        assert(strstr(message, "tcp_prio=2") != NULL);
        char expected_profile[32];
        (void)snprintf(expected_profile, sizeof(expected_profile),
                       "load_profile=%s", TEST_LOAD_PROFILE);
        assert(strstr(message, expected_profile) != NULL);
        assert(strstr(message, "lifecycle_log=0") != NULL);
        atomic_store(&s_log_config_valid, true);
    }
    if (strstr(message, "display load profile=") != NULL)
    {
        char profile[16];
        unsigned tcp_required = 0U;
        assert(sscanf(message,
                      "display load profile=%15s tcp_required=%u",
                      profile, &tcp_required) == 2);
        assert(strcmp(profile, TEST_LOAD_PROFILE) == 0);
        atomic_store(&s_log_tcp_required, tcp_required);
        atomic_store(&s_log_load_valid, true);
    }
    const char *tcp_stats = strstr(message, " tcp_tx_bytes=");
    if (tcp_stats != NULL)
    {
        unsigned long long transmit_bytes = 0U;
        unsigned long long receive_bytes = 0U;
        unsigned long long target_bytes = 0U;
        unsigned long long active_us = 0U;
        unsigned long long down_ms = 0U;
        unsigned long long maximum_pacing_lag_us = 0U;
        unsigned rate_ok = 0U;
        unsigned reconnect_count = 0U;
        unsigned pacing_late_count = 0U;
        assert(sscanf(tcp_stats,
                      " tcp_tx_bytes=%llu tcp_rx_bytes=%llu tcp_target_bytes=%llu tcp_active_us=%llu tcp_rate_ok=%u tcp_reconnects=%u tcp_down_ms=%llu tcp_pacing_late=%u tcp_pacing_max_lag_us=%llu",
                      &transmit_bytes, &receive_bytes, &target_bytes,
                      &active_us, &rate_ok, &reconnect_count, &down_ms,
                      &pacing_late_count, &maximum_pacing_lag_us) == 9);
        atomic_store(&s_log_tcp_transmit_bytes, transmit_bytes);
        atomic_store(&s_log_tcp_receive_bytes, receive_bytes);
        atomic_store(&s_log_tcp_target_bytes, target_bytes);
        atomic_store(&s_log_tcp_active_us, active_us);
        atomic_store(&s_log_tcp_rate_ok, rate_ok);
        atomic_store(&s_log_tcp_reconnect_count, reconnect_count);
        atomic_store(&s_log_tcp_down_ms, down_ms);
        atomic_store(&s_log_tcp_pacing_late_count, pacing_late_count);
        atomic_store(&s_log_tcp_maximum_pacing_lag_us,
                     maximum_pacing_lag_us);
    }
    const char *sources = strstr(message, " control=0x");
    if (sources != NULL)
    {
        unsigned control_error = 0U;
        unsigned audio_error = 0U;
        unsigned tcp_error = 0U;
        assert(sscanf(sources,
                      " control=0x%x audio=0x%x tcp=0x%x",
                      &control_error, &audio_error, &tcp_error) == 3);
        atomic_store(&s_log_control_error, control_error);
        atomic_store(&s_log_audio_error, audio_error);
        atomic_store(&s_log_tcp_error, tcp_error);
    }
    const char *wifi = strstr(message, " wifi_disconnects=");
    if (wifi != NULL)
    {
        unsigned disconnect_count = 0U;
        assert(sscanf(wifi, " wifi_disconnects=%u", &disconnect_count) == 1);
        atomic_store(&s_log_wifi_disconnect_count, disconnect_count);
    }
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "host";
}

esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t callback, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    assert(msg_id == WIFI_SERVICE_MSG);
    assert(sub_type == WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT);
    assert(callback != NULL);
    assert(context == EVENT_BUS_DISPATCH_PUBLISHER);
    assert(out_handle != NULL);
    (void)pthread_mutex_lock(&s_event_bus_lock);
    assert(s_wifi_event_callback == NULL);
    s_wifi_event_callback = callback;
    s_wifi_event_user_data = user_data;
    *out_handle = 1U;
    (void)pthread_mutex_unlock(&s_event_bus_lock);
    atomic_fetch_add(&s_event_subscribe_count, 1U);
    return ESP_OK;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    assert(handle == 1U);
    atomic_fetch_add(&s_event_unsubscribe_count, 1U);
    if (atomic_exchange(&s_event_unsubscribe_fail_once, false))
    {
        return ESP_FAIL;
    }
    (void)pthread_mutex_lock(&s_event_bus_lock);
    if (s_wifi_event_callback == NULL)
    {
        (void)pthread_mutex_unlock(&s_event_bus_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_wifi_event_callback = NULL;
    s_wifi_event_user_data = NULL;
    (void)pthread_mutex_unlock(&s_event_bus_lock);
    return ESP_OK;
}

#if TEST_REQUIRES_TCP
static void _publish_wifi_state(wifi_service_state_t state)
{
    const wifi_service_status_snapshot_t snapshot =
    {
        .state = state,
    };
    (void)pthread_mutex_lock(&s_event_bus_lock);
    event_bus_cb_t callback = s_wifi_event_callback;
    void *user_data = s_wifi_event_user_data;
    (void)pthread_mutex_unlock(&s_event_bus_lock);
    assert(callback != NULL);
    callback(WIFI_SERVICE_MSG,
             WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
             &snapshot, sizeof(snapshot), user_data);
}
#endif

esp_err_t wifi_service_get_status(wifi_service_status_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = (wifi_service_state_t)atomic_load(&s_wifi_state);
    return ESP_OK;
}

esp_err_t audio_service_start(void)
{
    atomic_fetch_add(&s_audio_start_count, 1U);
    return ESP_OK;
}

esp_err_t audio_service_stop(void)
{
    const size_t delete_count = display_benchmark_host_port_delete_count();
    assert(delete_count > 0U);
    assert(strcmp(display_benchmark_host_port_deleted_name(delete_count - 1U),
                  "display_audio") == 0);
    atomic_fetch_add(&s_audio_stop_count, 1U);
    return ESP_OK;
}

esp_err_t audio_service_write(void *data, size_t bytes, size_t *written,
                              uint32_t timeout_ms)
{
    assert(data != NULL);
    assert(written != NULL);
    assert(timeout_ms > 0U);
    atomic_fetch_add(&s_audio_write_count, 1U);
    *written = bytes;
    (void)usleep(100U);
    return ESP_OK;
}

esp_err_t audio_service_read(void *data, size_t bytes, size_t *read,
                             uint32_t timeout_ms)
{
    assert(data != NULL);
    assert(read != NULL);
    assert(timeout_ms > 0U);
    atomic_fetch_add(&s_audio_read_count, 1U);
    *read = bytes;
    (void)usleep(100U);
    return atomic_load(&s_audio_fail) ? ESP_FAIL : ESP_OK;
}

esp_err_t app_manager_display_diagnostics_begin_benchmark(
    uint32_t minimum_fps, size_t minimum_dma_largest)
{
    s_minimum_fps = minimum_fps;
    s_minimum_dma_largest = minimum_dma_largest;
    atomic_store(&s_profile_effect_mask, 0U);
    atomic_fetch_add(&s_diagnostics_begin_count, 1U);
    return ESP_OK;
}

esp_err_t app_manager_display_diagnostics_end_benchmark(
    app_manager_display_benchmark_report_t *report)
{
    assert(report != NULL);
    memset(report, 0, sizeof(*report));
    const test_report_quality_t quality =
        (test_report_quality_t)atomic_load(&s_report_quality);
    report->sample_count = 2U;
    report->minimum_fps = 35U;
    report->minimum_dma_largest = 40000U;
    report->frame_submit_count = 10U;
    report->sample_error_count =
        quality == TEST_REPORT_STABILITY_FAIL ? 1U : 0U;
    report->lvgl_lock_error_count = report->sample_error_count;
    report->maximum_fps_lock_wait_us =
        quality == TEST_REPORT_STABILITY_FAIL ? 250000U : 100U;
    report->passed = quality != TEST_REPORT_STABILITY_FAIL;

    const unsigned effect_mask = atomic_load(&s_profile_effect_mask);
    for (app_manager_transition_effect_t effect = APP_MANAGER_TRANSITION_NONE;
            effect < APP_MANAGER_TRANSITION_END; ++effect)
    {
        const size_t index = (size_t)(effect - APP_MANAGER_TRANSITION_NONE);
        app_manager_display_effect_benchmark_report_t *effect_report =
            &report->effects[index];
        effect_report->effect = effect;
        if ((effect_mask & (1U << (unsigned)effect)) == 0U)
        {
            continue;
        }
        effect_report->transition_start_count = 2U;
        effect_report->transition_complete_count = 2U;
#if CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION
        effect_report->snapshot_prepare_count = 2U;
        effect_report->snapshot_prepare_us = 80000U;
        effect_report->maximum_snapshot_prepare_us = 40000U;
        effect_report->snapshot_prepare_p95_us = 40000U;
#endif
        effect_report->active_duration_us = 1000000U;
        effect_report->interval_count = 100U;
        effect_report->render_count = 30U;
        effect_report->render_us = 400000U;
        effect_report->maximum_render_us = 16000U;
        effect_report->flush_count = 240U;
        effect_report->flush_pixel_count = 1000000U;
        effect_report->flush_us = 80000U;
        effect_report->maximum_flush_us = 1000U;
        effect_report->flush_wait_count = 30U;
        effect_report->flush_wait_us = 100000U;
        effect_report->maximum_flush_wait_us = 5000U;
        effect_report->panel_submit_count = 240U;
        effect_report->panel_pixel_count = 1000000U;
        effect_report->submit_wait_us = 400000U;
        effect_report->maximum_submit_wait_us = 17000U;

        if (quality == TEST_REPORT_FLOOR)
        {
            effect_report->active_frame_count = 25U;
            effect_report->interval_within_target_count = 80U;
            effect_report->maximum_consecutive_long_intervals = 2U;
            effect_report->interval_p50_us = 40000U;
            effect_report->interval_p95_us = 50000U;
            effect_report->interval_p99_us = 66667U;
            effect_report->maximum_interval_us = 100000U;
        }
        else if (quality == TEST_REPORT_SNAPSHOT_PREPARE_SLOW ||
                 (quality == TEST_REPORT_SNAPSHOT_PREPARE_SLOW_AUXILIARY &&
                  effect == APP_MANAGER_TRANSITION_COVER_LEFT))
        {
            effect_report->snapshot_prepare_p95_us =
                APP_MANAGER_DISPLAY_SNAPSHOT_PREPARE_TARGET_US + 1U;
            effect_report->active_frame_count = 30U;
            effect_report->interval_within_target_count = 95U;
            effect_report->maximum_consecutive_long_intervals = 1U;
            effect_report->interval_p50_us = 32000U;
            effect_report->interval_p95_us = 33333U;
            effect_report->interval_p99_us = 41667U;
            effect_report->maximum_interval_us = 50000U;
        }
        else if (quality == TEST_REPORT_SNAPSHOT_FALLBACK_WITHOUT_START &&
                 effect == APP_MANAGER_TRANSITION_REVEAL_RIGHT)
        {
            effect_report->transition_start_count = 0U;
            effect_report->transition_complete_count = 0U;
            effect_report->snapshot_prepare_count = 1U;
            effect_report->snapshot_prepare_us = 40000U;
            effect_report->snapshot_fallback_count = 1U;
            effect_report->active_frame_count = 30U;
            effect_report->interval_within_target_count = 95U;
            effect_report->maximum_consecutive_long_intervals = 1U;
            effect_report->interval_p50_us = 32000U;
            effect_report->interval_p95_us = 33333U;
            effect_report->interval_p99_us = 41667U;
            effect_report->maximum_interval_us = 50000U;
        }
        else if (quality == TEST_REPORT_SNAPSHOT_FALLBACK ||
                 (quality == TEST_REPORT_SNAPSHOT_FALLBACK_AUXILIARY &&
                  effect == APP_MANAGER_TRANSITION_REVEAL_RIGHT))
        {
            effect_report->snapshot_fallback_count = 1U;
            effect_report->active_frame_count = 30U;
            effect_report->interval_within_target_count = 95U;
            effect_report->maximum_consecutive_long_intervals = 1U;
            effect_report->interval_p50_us = 32000U;
            effect_report->interval_p95_us = 33333U;
            effect_report->interval_p99_us = 41667U;
            effect_report->maximum_interval_us = 50000U;
        }
        else if (quality == TEST_REPORT_FAIL)
        {
            effect_report->active_frame_count = 24U;
            effect_report->interval_within_target_count = 70U;
            effect_report->maximum_consecutive_long_intervals = 3U;
            effect_report->interval_p50_us = 42000U;
            effect_report->interval_p95_us = 51000U;
            effect_report->interval_p99_us = 70000U;
            effect_report->maximum_interval_us = 110000U;
        }
        else
        {
            effect_report->active_frame_count = 30U;
            effect_report->interval_within_target_count = 95U;
            effect_report->maximum_consecutive_long_intervals = 1U;
            effect_report->interval_p50_us = 32000U;
            effect_report->interval_p95_us = 33333U;
            effect_report->interval_p99_us = 41667U;
            effect_report->maximum_interval_us = 50000U;
        }
        report->transition_start_count +=
            effect_report->transition_start_count;
        report->transition_complete_count +=
            effect_report->transition_complete_count;
        report->snapshot_prepare_count += effect_report->snapshot_prepare_count;
        report->snapshot_prepare_us += effect_report->snapshot_prepare_us;
        if (effect_report->maximum_snapshot_prepare_us >
                report->maximum_snapshot_prepare_us)
        {
            report->maximum_snapshot_prepare_us =
                effect_report->maximum_snapshot_prepare_us;
        }
        if (effect_report->snapshot_prepare_p95_us >
                report->snapshot_prepare_p95_us)
        {
            report->snapshot_prepare_p95_us =
                effect_report->snapshot_prepare_p95_us;
        }
        report->snapshot_fallback_count += effect_report->snapshot_fallback_count;
        report->active_frame_count += effect_report->active_frame_count;
        report->active_duration_us += effect_report->active_duration_us;
        report->interval_count += effect_report->interval_count;
        report->interval_within_target_count +=
            effect_report->interval_within_target_count;
        report->render_count += effect_report->render_count;
        report->render_us += effect_report->render_us;
        report->flush_count += effect_report->flush_count;
        report->flush_pixel_count += effect_report->flush_pixel_count;
        report->flush_us += effect_report->flush_us;
        report->flush_wait_count += effect_report->flush_wait_count;
        report->flush_wait_us += effect_report->flush_wait_us;
        report->panel_submit_count += effect_report->panel_submit_count;
        report->panel_pixel_count += effect_report->panel_pixel_count;
        report->submit_wait_us += effect_report->submit_wait_us;
    }
    atomic_fetch_add(&s_diagnostics_end_count, 1U);
    return ESP_OK;
}

esp_err_t app_manager_display_diagnostics_wait_for_presentation(
    uint32_t timeout_ms)
{
    assert(timeout_ms == 1000U);
    atomic_fetch_add(&s_presentation_wait_count, 1U);
    return atomic_load(&s_presentation_wait_result);
}

esp_err_t app_manager_pm_notify_user_activity(void)
{
    atomic_fetch_add(&s_activity_count, 1U);
    return ESP_OK;
}

esp_err_t app_manager_navigate(const app_manager_nav_request_t *request,
                               uint32_t timeout_ms)
{
    assert(request != NULL);
    assert(timeout_ms == APP_MANAGER_TRANSITION_MAX_DURATION_MS + 1000U);
    assert(request->transition.duration_ms ==
           APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS);
    atomic_fetch_or(&s_profile_effect_mask,
                    1U << (unsigned)request->transition.effect);
#if TEST_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
    assert(request->operation == APP_MANAGER_NAV_OP_RUN);
    assert(strcmp(request->app_id, APP_MANAGER_ID_HOME) == 0 ||
           strcmp(request->app_id, APP_MANAGER_ID_SETTINGS) == 0);
    if (request->transition.effect == APP_MANAGER_TRANSITION_FADE)
    {
        atomic_store(&s_saw_fade, true);
    }
    else if (request->transition.effect == APP_MANAGER_TRANSITION_PUSH_LEFT)
    {
        atomic_store(&s_saw_push_left, true);
    }
    else if (request->transition.effect == APP_MANAGER_TRANSITION_PUSH_RIGHT)
    {
        atomic_store(&s_saw_push_right, true);
    }
    else if (request->transition.effect == APP_MANAGER_TRANSITION_COVER_LEFT)
    {
        atomic_store(&s_saw_cover_left, true);
    }
    else
    {
        assert(request->transition.effect ==
               APP_MANAGER_TRANSITION_REVEAL_RIGHT);
        atomic_store(&s_saw_reveal_right, true);
    }
#else
    if (request->transition.effect == APP_MANAGER_TRANSITION_FADE)
    {
        assert(request->operation == APP_MANAGER_NAV_OP_RUN);
        assert(strcmp(request->app_id, APP_MANAGER_ID_HOME) == 0 ||
               strcmp(request->app_id, APP_MANAGER_ID_SETTINGS) == 0);
        atomic_store(&s_saw_fade, true);
    }
    else if (request->transition.effect == APP_MANAGER_TRANSITION_PUSH_LEFT)
    {
        assert(request->operation == APP_MANAGER_NAV_OP_OPEN_PAGE);
        assert(strcmp(request->app_id, APP_MANAGER_ID_SETTINGS) == 0);
        assert(strcmp(request->page_id, "about") == 0);
        atomic_store(&s_saw_push_left, true);
    }
    else
    {
        assert(request->transition.effect == APP_MANAGER_TRANSITION_PUSH_RIGHT);
        assert(request->operation == APP_MANAGER_NAV_OP_BACK);
        atomic_store(&s_saw_push_right, true);
    }
#endif
    atomic_fetch_add(&s_navigation_count, 1U);
    (void)usleep(1000U);
    return ESP_OK;
}

static void *_echo_server_task(void *arg)
{
    echo_server_t *server = arg;
    const int listener = atomic_load(&server->listener);
    uint8_t buffer[2048];
    while (atomic_load(&server->listener) >= 0)
    {
        int connection = accept(listener, NULL, NULL);
        if (connection < 0)
        {
            break;
        }
        atomic_store(&server->connection, connection);
        const unsigned connection_count =
            atomic_fetch_add(&server->connection_count, 1U) + 1U;
        const bool reset_connection =
            connection_count == 1U &&
            atomic_load(&server->reset_first_connection);
        while (connection >= 0)
        {
            const ssize_t received = recv(connection, buffer,
                                          sizeof(buffer), 0);
            if (received <= 0 || reset_connection)
            {
                break;
            }
            if (atomic_load(&server->corrupt))
            {
                buffer[0] ^= 0xFFU;
            }
            const unsigned response_delay_us = atomic_exchange(
                                                   &server->first_response_delay_us, 0U);
            if (response_delay_us > 0U)
            {
                (void)usleep(response_delay_us);
            }
            size_t offset = 0U;
            while (offset < (size_t)received)
            {
                const ssize_t sent = send(connection, buffer + offset,
                                          (size_t)received - offset,
                                          MSG_NOSIGNAL);
                if (sent <= 0)
                {
                    connection = -1;
                    break;
                }
                offset += (size_t)sent;
            }
        }
        const int owned = atomic_exchange(&server->connection, -1);
        if (owned >= 0)
        {
            if (reset_connection)
            {
                const struct linger linger =
                {
                    .l_onoff = 1,
                    .l_linger = 0,
                };
                (void)setsockopt(owned, SOL_SOCKET, SO_LINGER, &linger,
                                 sizeof(linger));
            }
            (void)close(owned);
        }
    }
    return NULL;
}

static void _echo_server_start(echo_server_t *server, bool corrupt,
                               bool reset_first_connection,
                               unsigned first_response_delay_us)
{
    memset(server, 0, sizeof(*server));
    atomic_store(&server->listener, -1);
    atomic_store(&server->connection, -1);
    atomic_store(&server->corrupt, corrupt);
    atomic_store(&server->reset_first_connection, reset_first_connection);
    atomic_store(&server->first_response_delay_us, first_response_delay_us);
    atomic_store(&server->connection_count, 0U);
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener >= 0);
    const int reuse = 1;
    assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                      sizeof(reuse)) == 0);
    const struct sockaddr_in address =
    {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_DISPLAY_BENCHMARK_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    assert(bind(listener, (const struct sockaddr *)&address,
                sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    atomic_store(&server->listener, listener);
    assert(pthread_create(&server->thread, NULL, _echo_server_task, server) ==
           0);
}

static void _echo_server_stop(echo_server_t *server)
{
    const int connection = atomic_load(&server->connection);
    if (connection >= 0)
    {
        (void)shutdown(connection, SHUT_RDWR);
    }
    const int listener = atomic_exchange(&server->listener, -1);
    if (listener >= 0)
    {
        (void)shutdown(listener, SHUT_RDWR);
        (void)close(listener);
    }
    (void)pthread_join(server->thread, NULL);
}

static void _reset(void)
{
    display_benchmark_host_port_reset();
    (void)pthread_mutex_lock(&s_event_bus_lock);
    assert(s_wifi_event_callback == NULL);
    assert(s_wifi_event_user_data == NULL);
    (void)pthread_mutex_unlock(&s_event_bus_lock);
    atomic_store(&s_event_unsubscribe_fail_once, false);
    atomic_store(&s_event_subscribe_count, 0U);
    atomic_store(&s_event_unsubscribe_count, 0U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IDLE);
    atomic_store(&s_audio_fail, false);
    atomic_store(&s_audio_start_count, 0U);
    atomic_store(&s_audio_stop_count, 0U);
    atomic_store(&s_audio_write_count, 0U);
    atomic_store(&s_audio_read_count, 0U);
    atomic_store(&s_diagnostics_begin_count, 0U);
    atomic_store(&s_diagnostics_end_count, 0U);
    atomic_store(&s_navigation_count, 0U);
    atomic_store(&s_presentation_wait_count, 0U);
    atomic_store(&s_presentation_wait_result, ESP_OK);
    atomic_store(&s_activity_count, 0U);
    atomic_store(&s_saw_fade, false);
    atomic_store(&s_saw_push_left, false);
    atomic_store(&s_saw_push_right, false);
    atomic_store(&s_saw_cover_left, false);
    atomic_store(&s_saw_reveal_right, false);
    atomic_store(&s_profile_effect_mask, 0U);
    atomic_store(&s_report_quality, TEST_REPORT_TARGET);
    atomic_store(&s_log_result, 0U);
    atomic_store(&s_log_performance, 0U);
    atomic_store(&s_log_effect_count, 0U);
    atomic_store(&s_log_sample_error_count, UINT32_MAX);
    atomic_store(&s_log_lvgl_lock_error_count, UINT32_MAX);
    atomic_store(&s_log_fps_read_error_count, UINT32_MAX);
    atomic_store(&s_log_maximum_fps_lock_wait_us, UINT32_MAX);
    atomic_store(&s_log_frame_submit_count, UINT32_MAX);
    atomic_store(&s_log_has_legacy_fields, false);
    atomic_store(&s_log_config_valid, false);
    atomic_store(&s_log_load_valid, false);
    atomic_store(&s_log_second_profile_effect_count, 0U);
    atomic_store(&s_log_tcp_required, UINT32_MAX);
    atomic_store(&s_log_control_error, UINT32_MAX);
    atomic_store(&s_log_audio_error, UINT32_MAX);
    atomic_store(&s_log_tcp_error, UINT32_MAX);
    atomic_store(&s_log_tcp_transmit_bytes, UINT64_MAX);
    atomic_store(&s_log_tcp_receive_bytes, UINT64_MAX);
    atomic_store(&s_log_tcp_target_bytes, UINT64_MAX);
    atomic_store(&s_log_tcp_active_us, UINT64_MAX);
    atomic_store(&s_log_tcp_down_ms, UINT64_MAX);
    atomic_store(&s_log_tcp_maximum_pacing_lag_us, UINT64_MAX);
    atomic_store(&s_log_tcp_rate_ok, UINT32_MAX);
    atomic_store(&s_log_tcp_reconnect_count, UINT32_MAX);
    atomic_store(&s_log_tcp_pacing_late_count, UINT32_MAX);
    atomic_store(&s_log_wifi_disconnect_count, UINT32_MAX);
    atomic_store(&s_heap_allocate_count, 0U);
    atomic_store(&s_heap_free_count, 0U);
    atomic_store(&s_heap_maximum_allocation, 0U);
    s_minimum_fps = 0U;
    s_minimum_dma_largest = 0U;
}

static void _test_invalid_config(void)
{
    display_benchmark_config_t config = s_benchmark_config;
    assert((display_benchmark_start)(NULL) == ESP_ERR_INVALID_ARG);

    config.mode = (display_benchmark_mode_t)99;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config = s_benchmark_config;
    config.stress_duration_sec = 9U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config.stress_duration_sec = 28801U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config = s_benchmark_config;
    config.effect_duration_sec = 4U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config.effect_duration_sec = 301U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config = s_benchmark_config;
    config.load = (display_benchmark_load_t)99;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config = s_benchmark_config;
    config.ipv4_host = NULL;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config.ipv4_host = "";
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config.ipv4_host = "300.1.1.1";
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config.ipv4_host = "127.000.000.001x";
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config = s_benchmark_config;
    config.port = 0U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config = s_benchmark_config;
    config.rate_kbit_s = 63U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    config.rate_kbit_s = 20001U;
    assert((display_benchmark_start)(&config) == ESP_ERR_INVALID_ARG);
    assert(display_benchmark_stop() == ESP_OK);
}

static void _wait_for_counter(atomic_uint *counter, unsigned value)
{
    for (unsigned index = 0U; index < 2000U &&
            atomic_load(counter) < value; ++index)
    {
        (void)usleep(1000U);
    }
    assert(atomic_load(counter) >= value);
}

#if TEST_REQUIRES_TCP
static uint64_t _assert_tcp_target_matches_active_window(void)
{
    const uint64_t active_us = atomic_load(&s_log_tcp_active_us);
    assert(active_us >= (uint64_t)TEST_TCP_ACTIVE_US);
    const uint64_t target_bytes =
        (uint64_t)TEST_DISPLAY_BENCHMARK_TCP_RATE_KBIT_S *
        active_us / 8000U;
    assert(atomic_load(&s_log_tcp_target_bytes) == target_bytes);
    return target_bytes;
}
#endif

static void _test_runs_configured_load(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 0U);
    assert(display_benchmark_start() == ESP_OK);
    assert(display_benchmark_start() == ESP_ERR_INVALID_STATE);
#if TEST_REQUIRES_TCP
    (void)usleep(20000U);
    assert(atomic_load(&s_audio_start_count) == 0U);
    assert(atomic_load(&s_diagnostics_begin_count) == 0U);

    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
#endif
    _wait_for_counter(&s_log_result, 1U);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);

    assert(s_minimum_fps == 30U);
    assert(s_minimum_dma_largest == 14720U);
    assert(atomic_load(&s_audio_start_count) == TEST_REQUIRES_AUDIO);
    assert(atomic_load(&s_audio_stop_count) == TEST_REQUIRES_AUDIO);
    assert((atomic_load(&s_audio_write_count) > 0U) == TEST_REQUIRES_AUDIO);
    assert((atomic_load(&s_audio_read_count) > 0U) == TEST_REQUIRES_AUDIO);
    assert(atomic_load(&s_navigation_count) > 0U);
    assert(atomic_load(&s_activity_count) ==
           atomic_load(&s_navigation_count));
    assert(atomic_load(&s_presentation_wait_count) ==
           atomic_load(&s_navigation_count));
    assert(atomic_load(&s_saw_fade));
    assert(atomic_load(&s_saw_push_left));
    assert(atomic_load(&s_saw_push_right));
#if TEST_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
    assert(atomic_load(&s_saw_cover_left));
    assert(atomic_load(&s_saw_reveal_right));
    assert(atomic_load(&s_diagnostics_begin_count) == 2U);
    assert(atomic_load(&s_diagnostics_end_count) == 2U);
    assert(atomic_load(&s_log_effect_count) == 10U);
    assert(atomic_load(&s_log_second_profile_effect_count) == 5U);
#else
    assert(!atomic_load(&s_saw_cover_left));
    assert(!atomic_load(&s_saw_reveal_right));
    assert(atomic_load(&s_diagnostics_begin_count) == 1U);
    assert(atomic_load(&s_diagnostics_end_count) == 1U);
    assert(atomic_load(&s_log_effect_count) == 3U);
    assert(atomic_load(&s_log_second_profile_effect_count) == 3U);
#endif
    assert(atomic_load(&s_log_result) == 1U);
    assert(atomic_load(&s_log_performance) == 1U);
    assert(atomic_load(&s_log_sample_error_count) == 0U);
    assert(atomic_load(&s_log_lvgl_lock_error_count) == 0U);
    assert(atomic_load(&s_log_fps_read_error_count) == 0U);
    assert(atomic_load(&s_log_maximum_fps_lock_wait_us) == 100U);
    assert(atomic_load(&s_log_frame_submit_count) ==
           TEST_EXPECTED_FRAME_SUBMITS);
    assert(!atomic_load(&s_log_has_legacy_fields));
    assert(atomic_load(&s_log_config_valid));
    assert(atomic_load(&s_log_load_valid));
    assert(atomic_load(&s_log_tcp_required) == TEST_REQUIRES_TCP);
    assert(atomic_load(&s_log_control_error) == ESP_OK);
    assert(atomic_load(&s_log_audio_error) == ESP_OK);
    assert(atomic_load(&s_log_tcp_error) == ESP_OK);
#if TEST_REQUIRES_TCP
    const uint64_t target_bytes = _assert_tcp_target_matches_active_window();
    const uint64_t minimum_tcp_bytes = (target_bytes * 95U + 99U) / 100U;
    assert(atomic_load(&s_log_tcp_transmit_bytes) >= minimum_tcp_bytes);
    assert(atomic_load(&s_log_tcp_receive_bytes) >= minimum_tcp_bytes);
#else
    assert(atomic_load(&s_log_tcp_transmit_bytes) == 0U);
    assert(atomic_load(&s_log_tcp_receive_bytes) == 0U);
    assert(atomic_load(&s_log_tcp_target_bytes) == 0U);
    assert(atomic_load(&s_log_tcp_active_us) == 0U);
#endif
    assert(atomic_load(&s_log_tcp_rate_ok) == 1U);
    assert(atomic_load(&s_log_tcp_reconnect_count) == 0U);
    const unsigned pacing_late_count =
        atomic_load(&s_log_tcp_pacing_late_count);
    const uint64_t maximum_pacing_lag_us =
        atomic_load(&s_log_tcp_maximum_pacing_lag_us);
    assert((maximum_pacing_lag_us == 0U) == (pacing_late_count == 0U));
    assert(atomic_load(&s_log_wifi_disconnect_count) == 0U);
    assert(atomic_load(&s_event_subscribe_count) == TEST_REQUIRES_TCP);
    assert(atomic_load(&s_event_unsubscribe_count) == TEST_REQUIRES_TCP);
    const unsigned expected_resources = 1U + TEST_REQUIRES_AUDIO +
                                        TEST_REQUIRES_TCP;
    assert(display_benchmark_host_port_create_count() == expected_resources);
    assert(display_benchmark_host_port_delete_count() == expected_resources);
    assert(atomic_load(&s_heap_allocate_count) == expected_resources);
    assert(atomic_load(&s_heap_free_count) == expected_resources);
#if TEST_REQUIRES_TCP
    assert(atomic_load(&s_heap_maximum_allocation) ==
           TEST_TCP_PAYLOAD_BYTES * 2U);
#endif
    for (size_t index = 0U; index < expected_resources; ++index)
    {
        assert(display_benchmark_host_port_stack_depth(index) == 4096U);
        assert(display_benchmark_host_port_stack_caps(index) ==
               (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    assert(display_benchmark_host_port_priority(0U) == 1U);
#if TEST_REQUIRES_AUDIO
    assert(display_benchmark_host_port_priority(1U) == 1U);
#endif
#if TEST_REQUIRES_TCP
    assert(display_benchmark_host_port_priority(
               1U + TEST_REQUIRES_AUDIO) == 2U);
#else
    assert(atomic_load(&server.connection_count) == 0U);
#endif
    size_t delete_index = 0U;
#if TEST_REQUIRES_TCP
    assert(strcmp(display_benchmark_host_port_deleted_name(delete_index++),
                  "display_tcp") == 0);
#endif
#if TEST_REQUIRES_AUDIO
    assert(strcmp(display_benchmark_host_port_deleted_name(delete_index++),
                  "display_audio") == 0);
#endif
    assert(strcmp(display_benchmark_host_port_deleted_name(delete_index),
                  "display_bench") == 0);
    assert(display_benchmark_stop() == ESP_OK);
}

#if TEST_REQUIRES_AUDIO
static void _test_audio_failure_cleans_up(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 0U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    atomic_store(&s_audio_fail, true);
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);
    assert(atomic_load(&s_audio_stop_count) == 1U);
    assert(atomic_load(&s_log_result) == 2U);
    assert(atomic_load(&s_log_performance) == 3U);
    assert(atomic_load(&s_log_control_error) == ESP_OK);
    assert(atomic_load(&s_log_audio_error) == (unsigned)ESP_FAIL);
    assert(atomic_load(&s_log_tcp_error) == ESP_OK);
    const unsigned expected_resources = 1U + TEST_REQUIRES_AUDIO +
                                        TEST_REQUIRES_TCP;
    assert(display_benchmark_host_port_delete_count() == expected_resources);
    assert(atomic_load(&s_heap_allocate_count) == expected_resources);
    assert(atomic_load(&s_heap_free_count) == expected_resources);
}
#endif

#if TEST_REQUIRES_TCP
static void _test_tcp_failure_cleans_up(void)
{
    _reset();
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    const int64_t started_us = _monotonic_us();
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(_monotonic_us() - started_us >= TEST_BENCHMARK_TOTAL_US);
    assert(display_benchmark_stop() == ESP_OK);
    assert(atomic_load(&s_audio_stop_count) == TEST_REQUIRES_AUDIO);
    assert(atomic_load(&s_log_result) == 2U);
    assert(atomic_load(&s_log_performance) == 1U);
    assert(atomic_load(&s_log_control_error) == ESP_OK);
    assert(atomic_load(&s_log_audio_error) == ESP_OK);
    assert(atomic_load(&s_log_tcp_error) != ESP_OK);
    assert(atomic_load(&s_log_tcp_transmit_bytes) == 0U);
    assert(atomic_load(&s_log_tcp_receive_bytes) == 0U);
    (void)_assert_tcp_target_matches_active_window();
    assert(atomic_load(&s_log_tcp_rate_ok) == 0U);
    assert(atomic_load(&s_log_tcp_reconnect_count) > 0U);
    assert(atomic_load(&s_log_tcp_down_ms) > 0U);
    const unsigned expected_resources = 1U + TEST_REQUIRES_AUDIO +
                                        TEST_REQUIRES_TCP;
    assert(display_benchmark_host_port_delete_count() == expected_resources);
    assert(atomic_load(&s_heap_allocate_count) == expected_resources);
    assert(atomic_load(&s_heap_free_count) == expected_resources);
}

static void _test_tcp_reset_reconnects_without_shortening_benchmark(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, true, 0U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    const int64_t started_us = _monotonic_us();
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(_monotonic_us() - started_us >= TEST_BENCHMARK_TOTAL_US);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);

    assert(atomic_load(&server.connection_count) >= 2U);
    assert(atomic_load(&s_navigation_count) > 0U);
    assert(atomic_load(&s_log_result) == 2U);
    assert(atomic_load(&s_log_performance) == 1U);
    assert(atomic_load(&s_log_control_error) == ESP_OK);
    assert(atomic_load(&s_log_audio_error) == ESP_OK);
    assert(atomic_load(&s_log_tcp_error) != ESP_OK);
    assert(atomic_load(&s_log_tcp_transmit_bytes) > 0U);
    assert(atomic_load(&s_log_tcp_receive_bytes) > 0U);
    assert(atomic_load(&s_log_tcp_reconnect_count) >= 1U);
    assert(atomic_load(&s_log_tcp_down_ms) > 0U);
    const unsigned expected_resources = 1U + TEST_REQUIRES_AUDIO +
                                        TEST_REQUIRES_TCP;
    assert(display_benchmark_host_port_delete_count() == expected_resources);
    assert(atomic_load(&s_heap_allocate_count) == expected_resources);
    assert(atomic_load(&s_heap_free_count) == expected_resources);
}
#endif

#if TEST_DISPLAY_BENCHMARK_MODE_STRESS
static void _test_tcp_pacing_catches_up_without_reconnect(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 80000U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);

    assert(atomic_load(&server.connection_count) == 1U);
    assert(atomic_load(&s_log_result) == 1U);
    assert(atomic_load(&s_log_tcp_error) == ESP_OK);
    assert(atomic_load(&s_log_tcp_rate_ok) == 1U);
    assert(atomic_load(&s_log_tcp_reconnect_count) == 0U);
    assert(atomic_load(&s_log_tcp_pacing_late_count) > 0U);
    assert(atomic_load(&s_log_tcp_maximum_pacing_lag_us) > 40000U);
}
#endif

static void _test_report_classification(test_report_quality_t quality,
                                        unsigned expected_stability,
                                        unsigned expected_performance)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 0U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    atomic_store(&s_report_quality, quality);
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);

    assert(atomic_load(&s_log_result) == expected_stability);
    assert(atomic_load(&s_log_performance) == expected_performance);
    assert(atomic_load(&s_log_lvgl_lock_error_count) ==
           (quality == TEST_REPORT_STABILITY_FAIL ?
            TEST_EXPECTED_LOCK_ERRORS : 0U));
    assert(atomic_load(&s_log_fps_read_error_count) == 0U);
    assert(atomic_load(&s_log_maximum_fps_lock_wait_us) ==
           (quality == TEST_REPORT_STABILITY_FAIL ? 250000U : 100U));
    assert(atomic_load(&s_log_tcp_error) == ESP_OK);
    assert(atomic_load(&s_log_tcp_rate_ok) == 1U);
    assert(atomic_load(&s_log_tcp_reconnect_count) == 0U);
}

#if TEST_REQUIRES_TCP
static void _test_wifi_disconnect_marks_stability_without_shortening(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 0U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    const int64_t started_us = _monotonic_us();
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_navigation_count, 5U);
    _publish_wifi_state(WIFI_SERVICE_STATE_IDLE);
    _publish_wifi_state(WIFI_SERVICE_STATE_IP_READY);
    _wait_for_counter(&s_log_result, 1U);
    assert(_monotonic_us() - started_us >= TEST_BENCHMARK_TOTAL_US);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);

    assert(atomic_load(&s_log_result) == 2U);
    assert(atomic_load(&s_log_performance) == 1U);
    assert(atomic_load(&s_log_control_error) == (unsigned)ESP_FAIL);
    assert(atomic_load(&s_log_wifi_disconnect_count) == 1U);
}
#endif

static void _test_task_create_failure_releases_subscription(void)
{
    _reset();
    display_benchmark_host_port_fail_next_create();
    assert(display_benchmark_start() == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_event_subscribe_count) == TEST_REQUIRES_TCP);
    assert(atomic_load(&s_event_unsubscribe_count) == TEST_REQUIRES_TCP);
    assert(display_benchmark_host_port_create_count() == 0U);
    assert(display_benchmark_stop() == ESP_OK);

    assert(display_benchmark_start() == ESP_OK);
    (void)usleep(10000U);
    assert(display_benchmark_stop() == ESP_OK);
    assert(atomic_load(&s_event_subscribe_count) == 2U * TEST_REQUIRES_TCP);
    assert(atomic_load(&s_event_unsubscribe_count) == 2U * TEST_REQUIRES_TCP);
    assert(display_benchmark_host_port_create_count() == 1U);
    assert(display_benchmark_host_port_delete_count() == 1U);
}

static void _test_presentation_wait_failure_cleans_up(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 0U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    atomic_store(&s_presentation_wait_result, ESP_ERR_TIMEOUT);
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);

    assert(atomic_load(&s_navigation_count) == 1U);
    assert(atomic_load(&s_presentation_wait_count) == 1U);
    assert(atomic_load(&s_log_result) == 2U);
    assert(atomic_load(&s_log_performance) == 3U);
    assert(atomic_load(&s_log_control_error) == (unsigned)ESP_ERR_TIMEOUT);
}

#if TEST_REQUIRES_TCP
static void _test_tcp_worker_create_failure_fails_rate(void)
{
    _reset();
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    display_benchmark_host_port_fail_create_on(1U + TEST_REQUIRES_AUDIO);
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&s_log_result, 1U);
    assert(display_benchmark_stop() == ESP_OK);

    assert(atomic_load(&s_log_result) == 2U);
    assert(atomic_load(&s_log_performance) == 3U);
    assert(atomic_load(&s_log_control_error) == (unsigned)ESP_ERR_NO_MEM);
    assert(atomic_load(&s_log_tcp_error) == ESP_OK);
    assert(atomic_load(&s_log_tcp_active_us) == 0U);
    assert(atomic_load(&s_log_tcp_target_bytes) == 0U);
    assert(atomic_load(&s_log_tcp_rate_ok) == 0U);
    assert(atomic_load(&s_audio_start_count) == TEST_REQUIRES_AUDIO);
    assert(atomic_load(&s_audio_stop_count) == TEST_REQUIRES_AUDIO);
    assert(display_benchmark_host_port_create_count() ==
           1U + TEST_REQUIRES_AUDIO);
    assert(display_benchmark_host_port_delete_count() ==
           1U + TEST_REQUIRES_AUDIO);
    assert(atomic_load(&s_event_subscribe_count) == 1U);
    assert(atomic_load(&s_event_unsubscribe_count) == 1U);
}

static void _test_unsubscribe_failure_is_retryable(void)
{
    _reset();
    assert(display_benchmark_start() == ESP_OK);
    (void)usleep(10000U);
    atomic_store(&s_event_unsubscribe_fail_once, true);
    assert(display_benchmark_stop() == ESP_FAIL);
    assert(display_benchmark_start() == ESP_ERR_INVALID_STATE);
    assert(atomic_load(&s_event_unsubscribe_count) == 1U);
    assert(display_benchmark_stop() == ESP_OK);
    assert(atomic_load(&s_event_unsubscribe_count) == 2U);
    assert(display_benchmark_host_port_create_count() == 1U);
    assert(display_benchmark_host_port_delete_count() == 1U);
}

static void _test_stop_while_tcp_connected(void)
{
    _reset();
    echo_server_t server;
    _echo_server_start(&server, false, false, 50000U);
    atomic_store(&s_wifi_state, WIFI_SERVICE_STATE_IP_READY);
    assert(display_benchmark_start() == ESP_OK);
    _wait_for_counter(&server.connection_count, 1U);
    assert(display_benchmark_stop() == ESP_OK);
    _echo_server_stop(&server);
    const unsigned expected_resources = 1U + TEST_REQUIRES_AUDIO +
                                        TEST_REQUIRES_TCP;
    assert(display_benchmark_host_port_create_count() == expected_resources);
    assert(display_benchmark_host_port_delete_count() == expected_resources);
    assert(atomic_load(&s_event_subscribe_count) == 1U);
    assert(atomic_load(&s_event_unsubscribe_count) == 1U);
}

static void _test_stop_while_waiting_for_ip(void)
{
    _reset();
    assert(display_benchmark_start() == ESP_OK);
    (void)usleep(20000U);
    assert(display_benchmark_stop() == ESP_OK);
    assert(atomic_load(&s_audio_start_count) == 0U);
    assert(atomic_load(&s_diagnostics_begin_count) == 0U);
    assert(atomic_load(&s_navigation_count) == 0U);
    assert(!atomic_load(&s_log_config_valid));
    assert(display_benchmark_host_port_create_count() == 1U);
    assert(display_benchmark_host_port_delete_count() == 1U);
    assert(atomic_load(&s_heap_allocate_count) == 0U);
    assert(atomic_load(&s_heap_free_count) == 0U);
}
#endif

int main(void)
{
    _test_invalid_config();
    _test_runs_configured_load();
#if TEST_REQUIRES_AUDIO
    _test_audio_failure_cleans_up();
#endif
#if TEST_REQUIRES_TCP
    _test_tcp_failure_cleans_up();
    _test_tcp_reset_reconnects_without_shortening_benchmark();
#endif
#if TEST_DISPLAY_BENCHMARK_MODE_STRESS
    _test_tcp_pacing_catches_up_without_reconnect();
#endif
#if TEST_REQUIRES_TCP
    _test_wifi_disconnect_marks_stability_without_shortening();
#endif
    _test_report_classification(TEST_REPORT_FLOOR, 1U, 2U);
    _test_report_classification(TEST_REPORT_FAIL, 1U, 3U);
    _test_report_classification(TEST_REPORT_STABILITY_FAIL, 2U, 1U);
#if CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION
    _test_report_classification(TEST_REPORT_SNAPSHOT_PREPARE_SLOW, 1U, 3U);
    _test_report_classification(TEST_REPORT_SNAPSHOT_FALLBACK, 1U, 3U);
#if TEST_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
    _test_report_classification(TEST_REPORT_SNAPSHOT_PREPARE_SLOW_AUXILIARY,
                                1U, 3U);
    _test_report_classification(TEST_REPORT_SNAPSHOT_FALLBACK_AUXILIARY,
                                1U, 3U);
    _test_report_classification(TEST_REPORT_SNAPSHOT_FALLBACK_WITHOUT_START,
                                1U, 3U);
#endif
#endif
    _test_task_create_failure_releases_subscription();
    _test_presentation_wait_failure_cleans_up();
#if TEST_REQUIRES_TCP
    _test_tcp_worker_create_failure_fails_rate();
    _test_unsubscribe_failure_is_retryable();
    _test_stop_while_tcp_connected();
    _test_stop_while_waiting_for_ip();
#endif
    return 0;
}

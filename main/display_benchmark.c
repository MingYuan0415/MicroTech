#define DBG_TAG "display_bench"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "display_benchmark.h"

#if CONFIG_MAIN_DISPLAY_BENCHMARK

#include "app_manager.h"
#include "app_manager_display_diagnostics.h"
#include "audio_service.h"
#include "connectivity_manager.h"
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    #include "provisioning_service.h"
#endif

#include "esp_heap_caps.h"
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    #include "esp_memory_utils.h"
#endif
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define DISPLAY_BENCHMARK_SUPERVISOR_STACK       4096U
#define DISPLAY_BENCHMARK_AUDIO_STACK            4096U
#define DISPLAY_BENCHMARK_TCP_STACK              4096U
#define DISPLAY_BENCHMARK_SUPERVISOR_PRIORITY    1U
#define DISPLAY_BENCHMARK_AUDIO_PRIORITY         1U
#define DISPLAY_BENCHMARK_TCP_PRIORITY           2U
#define DISPLAY_BENCHMARK_DMA_MAX_FULL_LINES    44U
#define DISPLAY_BENCHMARK_TASK_CAPS              \
    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#ifndef DISPLAY_BENCHMARK_WIFI_POLL_MS
    #define DISPLAY_BENCHMARK_WIFI_POLL_MS       500U
#endif
#ifndef DISPLAY_BENCHMARK_IO_TIMEOUT_MS
    #define DISPLAY_BENCHMARK_IO_TIMEOUT_MS      1000U
#endif
#ifndef DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS
    #define DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS   250U
#endif
#ifndef DISPLAY_BENCHMARK_TCP_RECONNECT_MS
    #define DISPLAY_BENCHMARK_TCP_RECONNECT_MS   1000U
#endif
#define DISPLAY_BENCHMARK_PRESENTATION_TIMEOUT_MS 1000U
#define DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES \
    ((CONFIG_LWIP_TCP_SND_BUF_DEFAULT / CONFIG_LWIP_TCP_MSS) * \
     CONFIG_LWIP_TCP_MSS)
#define DISPLAY_BENCHMARK_TCP_MINIMUM_PERCENT      95U
#define DISPLAY_BENCHMARK_FPS_CROSS_CHECK          30U
#define DISPLAY_BENCHMARK_MINIMUM_DMA_LARGEST     14720U
#define DISPLAY_BENCHMARK_TARGET_FPS                30U
#define DISPLAY_BENCHMARK_FLOOR_FPS                 25U
#define DISPLAY_BENCHMARK_TARGET_PERCENT            95U
#define DISPLAY_BENCHMARK_FLOOR_P95_US           50000U
#define DISPLAY_BENCHMARK_FLOOR_P99_US           66667U
#define DISPLAY_BENCHMARK_FLOOR_MAX_US          100000U
#define DISPLAY_BENCHMARK_RENDER_MINIMUM_HWM      4096U
#ifndef DISPLAY_BENCHMARK_PHASE_US
    #define DISPLAY_BENCHMARK_PHASE_US            10000000LL
#endif
#define DISPLAY_BENCHMARK_CHARACTERIZATION_EFFECT_COUNT 5U
#define DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_COUNT   2U

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
#define DISPLAY_BENCHMARK_STRESS_AUDIO_STACK             4096U
#define DISPLAY_BENCHMARK_STRESS_SAMPLER_STACK           4096U
#ifndef DISPLAY_BENCHMARK_STRESS_SAMPLE_MS
    #define DISPLAY_BENCHMARK_STRESS_SAMPLE_MS            500U
#endif
#ifndef DISPLAY_BENCHMARK_STRESS_TCP_CONNECT_MS
    #define DISPLAY_BENCHMARK_STRESS_TCP_CONNECT_MS     30000U
#endif
#ifndef DISPLAY_BENCHMARK_STRESS_PROVISIONING_MS
    #define DISPLAY_BENCHMARK_STRESS_PROVISIONING_MS   300000U
#endif
#ifndef DISPLAY_BENCHMARK_STRESS_CLEANUP_MS
    #define DISPLAY_BENCHMARK_STRESS_CLEANUP_MS         60000U
#endif
#define DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES       1024U
#define DISPLAY_BENCHMARK_STRESS_AUDIO_DEADLINE_US      64000U
#ifndef DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS
    #define DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS      250U
#endif
#ifndef DISPLAY_BENCHMARK_STRESS_HEAVY_DWELL_MS
    #define DISPLAY_BENCHMARK_STRESS_HEAVY_DWELL_MS      1000U
#endif
#ifndef DISPLAY_BENCHMARK_STRESS_WARMUP_REPETITIONS
    #define DISPLAY_BENCHMARK_STRESS_WARMUP_REPETITIONS     2U
#endif
#define DISPLAY_BENCHMARK_STRESS_TASK_MINIMUM_HWM        1024U
#define DISPLAY_BENCHMARK_STRESS_RENDER_MINIMUM_HWM      4096U
#define DISPLAY_BENCHMARK_STRESS_BLE_INTERVAL_US     10000000LL
#define DISPLAY_BENCHMARK_STRESS_PSRAM_WINDOW_US    300000000LL
#define DISPLAY_BENCHMARK_STRESS_TREND_EDGE_US       30000000LL
#define DISPLAY_BENCHMARK_STRESS_PSRAM_TREND_BYTES      65536U
#define DISPLAY_BENCHMARK_STRESS_INTERNAL_RECOVERY_BYTES 8192U
#define DISPLAY_BENCHMARK_STRESS_PSRAM_RECOVERY_BYTES   65536U
#define DISPLAY_BENCHMARK_STRESS_MINIMUM_PERCENT           95U
#define DISPLAY_BENCHMARK_STRESS_TASK_COUNT                10U
#define DISPLAY_BENCHMARK_STRESS_ROUTE_COUNT                8U
#define DISPLAY_BENCHMARK_STRESS_EFFECT_COUNT              13U
#define DISPLAY_BENCHMARK_STRESS_INTERNAL_CAPS \
        (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DISPLAY_BENCHMARK_STRESS_DMA_CAPS \
        (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DISPLAY_BENCHMARK_STRESS_PSRAM_CAPS \
        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#if CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED
    #define DISPLAY_BENCHMARK_COLOR_FORMAT "RGB565_SWAPPED"
#else
    #define DISPLAY_BENCHMARK_COLOR_FORMAT "RGB565"
#endif

#if CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION && \
    !CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED
    #define DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_ENABLED 1U
    #define DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_NAME "enabled"
#else
    #define DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_ENABLED 0U
    #define DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_NAME "n/a"
#endif

#if CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT
    #define DISPLAY_BENCHMARK_DIRECT_DMA_ENABLED 1U
#else
    #define DISPLAY_BENCHMARK_DIRECT_DMA_ENABLED 0U
#endif

#if CONFIG_BSP_DISPLAY_TE_SYNC
    #define DISPLAY_BENCHMARK_TE_ENABLED 1U
#else
    #define DISPLAY_BENCHMARK_TE_ENABLED 0U
#endif

#if defined(CONFIG_LV_OS_NONE) && CONFIG_LV_OS_NONE
#define DISPLAY_BENCHMARK_LVGL_OS_NAME       "none"
#define DISPLAY_BENCHMARK_DRAW_STACK_SIZE   0U
#define DISPLAY_BENCHMARK_DRAW_PRIORITY     0U
#else
#define DISPLAY_BENCHMARK_LVGL_OS_NAME       "freertos"
#define DISPLAY_BENCHMARK_DRAW_STACK_SIZE \
        CONFIG_LV_DRAW_THREAD_STACK_SIZE
#define DISPLAY_BENCHMARK_DRAW_PRIORITY CONFIG_LV_DRAW_THREAD_PRIO
#endif

#if defined(CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL) && \
    CONFIG_ESP_LVGL_ADAPTER_FREETYPE_SMALL_RENDER_POOL
    #define DISPLAY_BENCHMARK_FREETYPE_RENDER_POOL_SIZE 4096U
#else
    #define DISPLAY_BENCHMARK_FREETYPE_RENDER_POOL_SIZE 16384U
#endif

#if defined(CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG) && \
    CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG
    #define DISPLAY_BENCHMARK_LIFECYCLE_LOG_ENABLED 1U
#else
    #define DISPLAY_BENCHMARK_LIFECYCLE_LOG_ENABLED 0U
#endif

_Static_assert(CONFIG_LWIP_TCP_MSS > 0,
               "TCP benchmark requires a positive MSS");
_Static_assert(DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES > 0,
               "TCP send buffer must hold at least one MSS");
_Static_assert(DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES <=
               CONFIG_LWIP_TCP_SND_BUF_DEFAULT,
               "TCP benchmark payload exceeds the send buffer");
_Static_assert(DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES % CONFIG_LWIP_TCP_MSS == 0,
               "TCP benchmark payload must contain whole MSS segments");

typedef struct display_benchmark_worker
{
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
    void *context;
} display_benchmark_worker_t;

typedef enum display_benchmark_page
{
    DISPLAY_BENCHMARK_PAGE_HOME = 0,
    DISPLAY_BENCHMARK_PAGE_SETTINGS,
    DISPLAY_BENCHMARK_PAGE_ABOUT,
} display_benchmark_page_t;

typedef struct display_benchmark_tcp_transfer_result
{
    esp_err_t result;
    int socket_errno;
    size_t offset;
} display_benchmark_tcp_transfer_result_t;

typedef struct display_benchmark_tcp_report
{
    uint64_t transmit_bytes;
    uint64_t receive_bytes;
    uint64_t target_bytes;
    uint64_t active_duration_us;
    uint64_t interruption_us;
    uint64_t maximum_pacing_lag_us;
    uint32_t reconnect_count;
    uint32_t pacing_late_count;
    bool required;
    bool throughput_passed;
} display_benchmark_tcp_report_t;

typedef enum display_benchmark_performance
{
    DISPLAY_BENCHMARK_PERFORMANCE_FAIL = 0,
    DISPLAY_BENCHMARK_PERFORMANCE_FLOOR,
    DISPLAY_BENCHMARK_PERFORMANCE_TARGET,
} display_benchmark_performance_t;

#define DISPLAY_BENCHMARK_LOAD_DISPLAY_ONLY ((display_benchmark_load_t)0)

typedef struct display_benchmark_stability_summary
{
    uint32_t report_count;
    uint32_t sample_count;
    uint32_t sample_error_count;
    uint32_t lvgl_lock_error_count;
    uint32_t fps_read_error_count;
    uint32_t maximum_fps_lock_wait_us;
    size_t minimum_internal_free;
    size_t minimum_internal_largest;
    size_t minimum_dma_free;
    size_t minimum_dma_largest;
    size_t minimum_psram_free;
    size_t minimum_psram_largest;
    uint32_t minimum_render_stack_high_water;
    uint32_t dma_failure_count;
    uint32_t frame_submit_count;
    uint32_t submit_failure_count;
    uint32_t transition_cancel_count;
    uint32_t snapshot_prepare_count;
    uint64_t snapshot_prepare_us;
    uint32_t maximum_snapshot_prepare_us;
    uint32_t maximum_snapshot_prepare_p95_us;
    uint32_t snapshot_fallback_count;
    bool render_task_found;
    bool render_task_stack_in_psram;
    bool heap_sampled;
    bool diagnostics_passed;
} display_benchmark_stability_summary_t;

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
typedef enum display_benchmark_stress_phase
{
    DISPLAY_BENCHMARK_STRESS_PHASE_NONE = 0,
    DISPLAY_BENCHMARK_STRESS_PHASE_BEGIN,
    DISPLAY_BENCHMARK_STRESS_PHASE_LOAD_START,
    DISPLAY_BENCHMARK_STRESS_PHASE_PROVISIONING_WAIT,
    DISPLAY_BENCHMARK_STRESS_PHASE_WARMUP,
    DISPLAY_BENCHMARK_STRESS_PHASE_MEASURE,
    DISPLAY_BENCHMARK_STRESS_PHASE_CLEANUP,
    DISPLAY_BENCHMARK_STRESS_PHASE_END,
} display_benchmark_stress_phase_t;

typedef struct display_benchmark_stress_heap_sample
{
    int64_t elapsed_us;
    size_t psram_free;
} display_benchmark_stress_heap_sample_t;

typedef struct display_benchmark_stress_task_metric
{
    const char *name;
    uint32_t minimum_high_water;
    uint32_t sample_count;
    bool found_in_every_sample;
    bool stack_in_psram_in_every_sample;
} display_benchmark_stress_task_metric_t;

typedef struct display_benchmark_stress_route
{
    const char *app_id;
    const char *page_id;
    uint32_t dwell_ms;
} display_benchmark_stress_route_t;

typedef struct display_benchmark_stress_context
{
    display_benchmark_worker_t audio_tx_worker;
    display_benchmark_worker_t audio_rx_worker;
    display_benchmark_worker_t sampler_worker;
    display_benchmark_stress_heap_sample_t *heap_samples;
    size_t heap_sample_capacity;
    size_t heap_sample_count;
    display_benchmark_stress_task_metric_t tasks[
        DISPLAY_BENCHMARK_STRESS_TASK_COUNT];
    audio_service_config_t audio_config;
    audio_service_state_t original_audio_state;
    uint8_t original_volume_percent;
    bool original_muted;
    bool original_pa_enabled;
    bool audio_state_saved;
    atomic_int phase;
    atomic_bool sampler_enabled;
    atomic_int_fast64_t measure_start_us;
    atomic_uint_fast64_t audio_transmit_bytes;
    atomic_uint_fast64_t audio_receive_bytes;
    atomic_uint audio_transmit_short_count;
    atomic_uint audio_receive_short_count;
    atomic_uint audio_transmit_timeout_count;
    atomic_uint audio_receive_timeout_count;
    atomic_uint audio_transmit_error_count;
    atomic_uint audio_receive_error_count;
    atomic_uint audio_transmit_deadline_miss_count;
    atomic_uint audio_receive_deadline_miss_count;
    atomic_bool microphone_nonzero;
    size_t minimum_internal_free;
    size_t minimum_internal_largest;
    size_t minimum_dma_free;
    size_t minimum_dma_largest;
    size_t minimum_psram_free;
    size_t minimum_psram_largest;
    size_t warm_internal_free;
    size_t warm_psram_free;
    size_t cleanup_internal_free;
    size_t cleanup_psram_free;
    uint64_t maximum_ble_success_interval_us;
    uint64_t maximum_ble_success_idle_us;
    int64_t last_observed_snapshot_success_us;
    uint32_t ble_disconnect_count;
    uint32_t ble_reconnect_count;
    uint32_t route_visit_mask;
    uint32_t effect_visit_mask;
    bool ble_was_connected;
    bool heap_sampled;
} display_benchmark_stress_context_t;

static const app_manager_transition_effect_t s_stress_effects[
    DISPLAY_BENCHMARK_STRESS_EFFECT_COUNT] =
{
    APP_MANAGER_TRANSITION_FADE,
    APP_MANAGER_TRANSITION_COVER_LEFT,
    APP_MANAGER_TRANSITION_COVER_RIGHT,
    APP_MANAGER_TRANSITION_COVER_UP,
    APP_MANAGER_TRANSITION_COVER_DOWN,
    APP_MANAGER_TRANSITION_PUSH_LEFT,
    APP_MANAGER_TRANSITION_PUSH_RIGHT,
    APP_MANAGER_TRANSITION_PUSH_UP,
    APP_MANAGER_TRANSITION_PUSH_DOWN,
    APP_MANAGER_TRANSITION_REVEAL_LEFT,
    APP_MANAGER_TRANSITION_REVEAL_RIGHT,
    APP_MANAGER_TRANSITION_REVEAL_UP,
    APP_MANAGER_TRANSITION_REVEAL_DOWN,
};

static const display_benchmark_stress_route_t s_stress_routes[
    DISPLAY_BENCHMARK_STRESS_ROUTE_COUNT] =
{
    {APP_MANAGER_ID_HOME, "root", DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS},
    {APP_MANAGER_ID_SETTINGS, "root", DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS},
    {APP_MANAGER_ID_SETTINGS, "power", DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS},
    {APP_MANAGER_ID_SETTINGS, "about", DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS},
    {APP_MANAGER_ID_MENU, "root", DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS},
    {APP_MANAGER_ID_MENU, "clock", DISPLAY_BENCHMARK_STRESS_HEAVY_DWELL_MS},
    {APP_MANAGER_ID_MENU, "motion", DISPLAY_BENCHMARK_STRESS_HEAVY_DWELL_MS},
    {APP_MANAGER_ID_MENU, "storage", DISPLAY_BENCHMARK_STRESS_HEAVY_DWELL_MS},
};

static const char *const s_stress_task_names[
    DISPLAY_BENCHMARK_STRESS_TASK_COUNT] =
{
    "lvgl",
    "connectivity",
    "wifi_service",
    "provisioning",
    "nimble_host",
    "display_bench",
    "display_tcp",
    "stress_audio_tx",
    "stress_audio_rx",
    "stress_sampler",
};

static display_benchmark_stress_context_t *s_stress_context;
#endif

static const app_manager_transition_effect_t s_characterization_effects[
    DISPLAY_BENCHMARK_CHARACTERIZATION_EFFECT_COUNT] =
{
    APP_MANAGER_TRANSITION_FADE,
    APP_MANAGER_TRANSITION_PUSH_LEFT,
    APP_MANAGER_TRANSITION_PUSH_RIGHT,
    APP_MANAGER_TRANSITION_COVER_LEFT,
    APP_MANAGER_TRANSITION_REVEAL_RIGHT,
};

static TaskHandle_t s_supervisor_task;
static SemaphoreHandle_t s_supervisor_stopped;
static display_benchmark_worker_t s_audio_worker;
static display_benchmark_worker_t s_tcp_worker;
static atomic_bool s_stop_requested;
static atomic_bool s_load_stop_requested;
static atomic_int s_workload_error;
static atomic_int s_control_error;
static atomic_int s_audio_error;
static atomic_int s_tcp_error;
static atomic_uint_fast64_t s_tcp_transmit_bytes;
static atomic_uint_fast64_t s_tcp_receive_bytes;
static atomic_uint_fast64_t s_tcp_interruption_us;
static atomic_uint_fast64_t s_tcp_maximum_pacing_lag_us;
static atomic_int_fast64_t s_tcp_disconnected_since_us;
static atomic_uint s_tcp_reconnect_count;
static atomic_uint s_tcp_pacing_late_count;
static event_bus_sub_handle_t s_wifi_subscription;
static display_benchmark_config_t s_config;
static char s_ipv4_host[INET_ADDRSTRLEN];
static atomic_bool s_wifi_monitoring_active;
static atomic_bool s_wifi_was_ready;
static atomic_uint s_wifi_disconnect_count;
static atomic_bool s_tcp_connected;

static bool _display_benchmark_config_valid(
    const display_benchmark_config_t *config)
{
    struct in_addr address;
    bool valid = config != NULL &&
                 (config->mode == DISPLAY_BENCHMARK_MODE_STRESS ||
                  config->mode == DISPLAY_BENCHMARK_MODE_CHARACTERIZATION) &&
                 config->stress_duration_sec >= 10U &&
                 config->stress_duration_sec <= 28800U &&
                 config->effect_duration_sec >= 5U &&
                 config->effect_duration_sec <= 300U &&
                 (config->load == DISPLAY_BENCHMARK_LOAD_FULL ||
                  config->load == DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY ||
                  config->load == DISPLAY_BENCHMARK_LOAD_TCP_ONLY) &&
                 config->ipv4_host != NULL && config->ipv4_host[0] != '\0' &&
                 strlen(config->ipv4_host) < sizeof(s_ipv4_host) &&
                 inet_pton(AF_INET, config->ipv4_host, &address) == 1 &&
                 config->port != 0U && config->rate_kbit_s >= 64U &&
                 config->rate_kbit_s <= 20000U &&
                 config->ble_mode >= DISPLAY_BENCHMARK_BLE_OFF &&
                 config->ble_mode <= DISPLAY_BENCHMARK_BLE_SECURITY2_CONNECTED &&
                 config->app_workload >=
                 DISPLAY_BENCHMARK_APP_WORKLOAD_DISPLAY_ROUTES &&
                 config->app_workload <=
                 DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES &&
                 config->audio_volume_percent <= 100U;
    if (!valid)
    {
        return false;
    }
    if (config->ble_mode == DISPLAY_BENCHMARK_BLE_SECURITY2_CONNECTED)
    {
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
        valid = config->mode == DISPLAY_BENCHMARK_MODE_STRESS &&
                config->load == DISPLAY_BENCHMARK_LOAD_FULL &&
                config->app_workload ==
                DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES;
#else
        valid = false;
#endif
    }
    return valid;
}

static int64_t _display_benchmark_stress_duration_us(void)
{
#ifdef DISPLAY_BENCHMARK_DURATION_US
    return DISPLAY_BENCHMARK_DURATION_US;
#else
    return (int64_t)s_config.stress_duration_sec * 1000000LL;
#endif
}

static int64_t _display_benchmark_effect_duration_us(void)
{
#ifdef DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US
    return DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US;
#else
    return (int64_t)s_config.effect_duration_sec * 1000000LL;
#endif
}

static void _display_benchmark_record_stability_error(atomic_int *source,
        esp_err_t error);
static void _display_benchmark_check_wifi(void);

static void _display_benchmark_record_error(atomic_int *source,
        esp_err_t error)
{
    int expected = ESP_OK;
    if (error != ESP_OK)
    {
        (void)atomic_compare_exchange_strong_explicit(source, &expected,
                error, memory_order_relaxed, memory_order_relaxed);
        expected = ESP_OK;
        (void)atomic_compare_exchange_strong_explicit(&s_workload_error,
                &expected, error, memory_order_relaxed, memory_order_relaxed);
    }
}

static void _display_benchmark_record_tcp_error(esp_err_t error)
{
    int expected = ESP_OK;
    if (error != ESP_OK)
    {
        (void)atomic_compare_exchange_strong_explicit(&s_tcp_error, &expected,
                error, memory_order_relaxed, memory_order_relaxed);
    }
}

static void _display_benchmark_update_maximum(
    atomic_uint_fast64_t *maximum, uint64_t value)
{
    uint_fast64_t observed = atomic_load_explicit(maximum,
                             memory_order_relaxed);
    while (value > observed &&
            !atomic_compare_exchange_weak_explicit(
                maximum, &observed, value,
                memory_order_relaxed, memory_order_relaxed))
    {
    }
}

static void _display_benchmark_tcp_record_pacing_lag(int64_t wait_us)
{
    if (wait_us >= 0)
    {
        return;
    }
    const uint64_t lag_us = (uint64_t)(-wait_us);
    atomic_fetch_add_explicit(&s_tcp_pacing_late_count, 1U,
                              memory_order_relaxed);
    _display_benchmark_update_maximum(&s_tcp_maximum_pacing_lag_us, lag_us);
}

static bool _display_benchmark_should_stop(void)
{
    if (atomic_load_explicit(&s_stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&s_load_stop_requested, memory_order_acquire))
    {
        return true;
    }
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    if (s_stress_context != NULL &&
            atomic_load_explicit(&s_workload_error,
                                 memory_order_relaxed) != ESP_OK)
    {
        return true;
    }
#endif
    return false;
}

static void _display_benchmark_audio_task(void *arg)
{
    display_benchmark_worker_t *worker = arg;
    int16_t *buffers = heap_caps_malloc(sizeof(int16_t) * 1024U,
                                        DISPLAY_BENCHMARK_TASK_CAPS);
    if (buffers == NULL)
    {
        _display_benchmark_record_error(&s_audio_error, ESP_ERR_NO_MEM);
        goto exit;
    }
    int16_t *playback = buffers;
    int16_t *capture = buffers + 512U;
    uint32_t phase = 0U;

    while (!_display_benchmark_should_stop())
    {
        for (size_t frame = 0U; frame < 256U; ++frame)
        {
            const int16_t sample = (phase++ & 0x20U) != 0U ? 800 : -800;
            playback[frame * 2U] = sample;
            playback[frame * 2U + 1U] = sample;
        }

        size_t written = 0U;
        esp_err_t result = audio_service_write(playback,
                                               sizeof(int16_t) * 512U,
                                               &written,
                                               DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS);
        if (result != ESP_OK || written != sizeof(int16_t) * 512U)
        {
            _display_benchmark_record_error(&s_audio_error,
                                            result == ESP_OK ?
                                            ESP_ERR_INVALID_SIZE : result);
            break;
        }
        if (_display_benchmark_should_stop())
        {
            break;
        }

        size_t read = 0U;
        result = audio_service_read(capture, sizeof(int16_t) * 512U, &read,
                                    DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS);
        if (result != ESP_OK || read != sizeof(int16_t) * 512U)
        {
            _display_benchmark_record_error(&s_audio_error,
                                            result == ESP_OK ?
                                            ESP_ERR_INVALID_SIZE : result);
            break;
        }
    }

exit:
    heap_caps_free(buffers);
    (void)xSemaphoreGive(worker->stopped);
    vTaskSuspend(NULL);
}

static void _display_benchmark_tcp_mark_disconnected(int64_t now_us)
{
    atomic_store_explicit(&s_tcp_connected, false, memory_order_release);
    int_fast64_t expected = 0;
    (void)atomic_compare_exchange_strong_explicit(
        &s_tcp_disconnected_since_us, &expected, now_us,
        memory_order_relaxed, memory_order_relaxed);
}

static uint64_t _display_benchmark_tcp_mark_connected(int64_t now_us)
{
    atomic_store_explicit(&s_tcp_connected, true, memory_order_release);
    const int64_t disconnected_since_us = atomic_exchange_explicit(
            &s_tcp_disconnected_since_us, 0, memory_order_relaxed);
    if (disconnected_since_us <= 0 || now_us <= disconnected_since_us)
    {
        return 0U;
    }
    const uint64_t interruption_us = (uint64_t)(now_us -
                                     disconnected_since_us);
    atomic_fetch_add_explicit(&s_tcp_interruption_us, interruption_us,
                              memory_order_relaxed);
    return interruption_us;
}

static esp_err_t _display_benchmark_socket_wait(int socket_fd, bool writable,
        int64_t deadline_us, int *socket_errno)
{
    *socket_errno = 0;
    while (!_display_benchmark_should_stop() &&
            esp_timer_get_time() < deadline_us)
    {
        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        if (writable)
        {
            FD_SET(socket_fd, &write_set);
        }
        else
        {
            FD_SET(socket_fd, &read_set);
        }
        struct timeval timeout =
        {
            .tv_sec = 0,
            .tv_usec = 100000,
        };
        const int result = select(socket_fd + 1, &read_set, &write_set,
                                  NULL, &timeout);
        if (result > 0)
        {
            return ESP_OK;
        }
        if (result < 0 && errno != EINTR)
        {
            *socket_errno = errno;
            return ESP_FAIL;
        }
    }
    if (_display_benchmark_should_stop())
    {
        return ESP_OK;
    }
    *socket_errno = ETIMEDOUT;
    return ESP_ERR_TIMEOUT;
}

static display_benchmark_tcp_transfer_result_t
_display_benchmark_socket_connect(int socket_fd)
{
    display_benchmark_tcp_transfer_result_t transfer =
    {
        .result = ESP_OK,
    };
    struct sockaddr_in address =
    {
        .sin_family = AF_INET,
        .sin_port = htons(s_config.port),
    };
    if (inet_pton(AF_INET, s_config.ipv4_host,
                  &address.sin_addr) != 1)
    {
        transfer.result = ESP_ERR_INVALID_ARG;
        transfer.socket_errno = EINVAL;
        return transfer;
    }
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        transfer.result = ESP_FAIL;
        transfer.socket_errno = errno;
        return transfer;
    }
    if (connect(socket_fd, (const struct sockaddr *)&address,
                sizeof(address)) == 0)
    {
        return transfer;
    }
    if (errno != EINPROGRESS)
    {
        transfer.result = ESP_FAIL;
        transfer.socket_errno = errno;
        return transfer;
    }
    transfer.result = _display_benchmark_socket_wait(
                          socket_fd, true,
                          esp_timer_get_time() +
                          DISPLAY_BENCHMARK_IO_TIMEOUT_MS * 1000LL,
                          &transfer.socket_errno);
    if (transfer.result != ESP_OK || _display_benchmark_should_stop())
    {
        return transfer;
    }
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                   &error_size) < 0)
    {
        transfer.result = ESP_FAIL;
        transfer.socket_errno = errno;
    }
    else if (socket_error != 0)
    {
        transfer.result = ESP_FAIL;
        transfer.socket_errno = socket_error;
    }
    return transfer;
}

static display_benchmark_tcp_transfer_result_t
_display_benchmark_socket_transfer(int socket_fd,
                                   uint8_t *buffer, size_t size, bool send_data)
{
    display_benchmark_tcp_transfer_result_t transfer =
    {
        .result = ESP_OK,
    };
    const int64_t deadline_us = esp_timer_get_time() +
                                DISPLAY_BENCHMARK_IO_TIMEOUT_MS * 1000LL;
    while (transfer.offset < size && !_display_benchmark_should_stop())
    {
        transfer.result = _display_benchmark_socket_wait(
                              socket_fd, send_data, deadline_us,
                              &transfer.socket_errno);
        if (transfer.result != ESP_OK || _display_benchmark_should_stop())
        {
            break;
        }
        const ssize_t transferred = send_data ?
                                    send(socket_fd, buffer + transfer.offset,
                                         size - transfer.offset, 0) :
                                    recv(socket_fd, buffer + transfer.offset,
                                         size - transfer.offset, 0);
        if (transferred > 0)
        {
            transfer.offset += (size_t)transferred;
        }
        else if (transferred == 0)
        {
            transfer.result = ESP_FAIL;
            transfer.socket_errno = ECONNRESET;
            break;
        }
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            transfer.result = ESP_FAIL;
            transfer.socket_errno = errno;
            break;
        }
    }
    if (_display_benchmark_should_stop())
    {
        transfer.result = ESP_OK;
    }
    else if (transfer.offset != size && transfer.result == ESP_OK)
    {
        transfer.result = ESP_FAIL;
    }
    return transfer;
}

static void _display_benchmark_tcp_close(int *socket_fd)
{
    if (*socket_fd >= 0)
    {
        (void)shutdown(*socket_fd, SHUT_RDWR);
        (void)close(*socket_fd);
        *socket_fd = -1;
    }
}

static void _display_benchmark_tcp_log_failure(const char *stage,
        const display_benchmark_tcp_transfer_result_t *transfer,
        size_t bytes)
{
    LOG_W("TCP stage=%s failed result=0x%x errno=%d offset=%u bytes=%u reconnects=%u",
          stage, (unsigned)transfer->result, transfer->socket_errno,
          (unsigned)transfer->offset, (unsigned)bytes,
          (unsigned)atomic_load_explicit(&s_tcp_reconnect_count,
                                         memory_order_relaxed));
}

static void _display_benchmark_tcp_task(void *arg)
{
    display_benchmark_worker_t *worker = arg;
    uint8_t *buffers = heap_caps_malloc(
                           DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES * 2U,
                           DISPLAY_BENCHMARK_TASK_CAPS);
    int socket_fd = -1;
    bool connection_attempted = false;
    _display_benchmark_tcp_mark_disconnected(esp_timer_get_time());
    if (buffers == NULL)
    {
        _display_benchmark_record_tcp_error(ESP_ERR_NO_MEM);
        goto exit;
    }
    uint8_t *transmit = buffers;
    uint8_t *receive = buffers + DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES;
    for (size_t index = 0U;
            index < DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES; ++index)
    {
        transmit[index] = (uint8_t)index;
    }

    const int64_t period_us =
        (int64_t)DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES * 8LL * 1000LL /
        s_config.rate_kbit_s;
    int64_t next_transfer_us = 0;
    while (!_display_benchmark_should_stop())
    {
        if (socket_fd < 0)
        {
            if (connection_attempted)
            {
                atomic_fetch_add_explicit(&s_tcp_reconnect_count, 1U,
                                          memory_order_relaxed);
                vTaskDelay(pdMS_TO_TICKS(DISPLAY_BENCHMARK_TCP_RECONNECT_MS));
                if (_display_benchmark_should_stop())
                {
                    break;
                }
            }
            connection_attempted = true;
            socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket_fd < 0)
            {
                const display_benchmark_tcp_transfer_result_t failure =
                {
                    .result = ESP_FAIL,
                    .socket_errno = errno,
                };
                _display_benchmark_tcp_log_failure("connect", &failure, 0U);
                _display_benchmark_record_tcp_error(failure.result);
                continue;
            }
            const display_benchmark_tcp_transfer_result_t connection =
                _display_benchmark_socket_connect(socket_fd);
            if (connection.result != ESP_OK)
            {
                _display_benchmark_tcp_log_failure("connect", &connection,
                                                   0U);
                _display_benchmark_record_tcp_error(connection.result);
                _display_benchmark_tcp_close(&socket_fd);
                continue;
            }
            if (_display_benchmark_should_stop())
            {
                break;
            }
            const uint64_t interruption_us =
                _display_benchmark_tcp_mark_connected(esp_timer_get_time());
            LOG_I("TCP connected to %s:%u reconnects=%u interruption_ms=%llu",
                  s_config.ipv4_host,
                  (unsigned)s_config.port,
                  (unsigned)atomic_load_explicit(&s_tcp_reconnect_count,
                                                 memory_order_relaxed),
                  (unsigned long long)(interruption_us / 1000U));
            /* A connection starts a new pacing epoch. Scheduling lag within
             * that connection remains relative to this absolute timeline. */
            next_transfer_us = esp_timer_get_time();
        }

        int64_t transfer_start_us = esp_timer_get_time();
        display_benchmark_tcp_transfer_result_t transfer =
            _display_benchmark_socket_transfer(
                socket_fd, transmit, DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES,
                true);
        atomic_fetch_add_explicit(&s_tcp_transmit_bytes, transfer.offset,
                                  memory_order_relaxed);
        if (_display_benchmark_should_stop())
        {
            break;
        }
        if (transfer.result != ESP_OK)
        {
            _display_benchmark_tcp_log_failure(
                "send", &transfer, DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES);
            _display_benchmark_record_tcp_error(transfer.result);
            _display_benchmark_tcp_mark_disconnected(transfer_start_us);
            _display_benchmark_tcp_close(&socket_fd);
            continue;
        }

        transfer_start_us = esp_timer_get_time();
        transfer = _display_benchmark_socket_transfer(
                       socket_fd, receive,
                       DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES, false);
        atomic_fetch_add_explicit(&s_tcp_receive_bytes, transfer.offset,
                                  memory_order_relaxed);
        if (_display_benchmark_should_stop())
        {
            break;
        }
        if (transfer.result != ESP_OK)
        {
            _display_benchmark_tcp_log_failure(
                "recv", &transfer, DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES);
            _display_benchmark_record_tcp_error(transfer.result);
            _display_benchmark_tcp_mark_disconnected(transfer_start_us);
            _display_benchmark_tcp_close(&socket_fd);
            continue;
        }
        if (memcmp(transmit, receive,
                   DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES) != 0)
        {
            transfer.result = ESP_FAIL;
            transfer.socket_errno = 0;
            _display_benchmark_tcp_log_failure(
                "verify", &transfer, DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES);
            _display_benchmark_record_tcp_error(transfer.result);
            _display_benchmark_tcp_mark_disconnected(esp_timer_get_time());
            _display_benchmark_tcp_close(&socket_fd);
            continue;
        }

        next_transfer_us += period_us;
        const int64_t wait_us = next_transfer_us - esp_timer_get_time();
        _display_benchmark_tcp_record_pacing_lag(wait_us);
        if (wait_us > 1000LL)
        {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_us / 1000LL)));
        }
    }

exit:
    atomic_store_explicit(&s_tcp_connected, false, memory_order_release);
    _display_benchmark_tcp_close(&socket_fd);
    heap_caps_free(buffers);
    (void)xSemaphoreGive(worker->stopped);
    vTaskSuspend(NULL);
}

static esp_err_t _display_benchmark_worker_start(
    display_benchmark_worker_t *worker, TaskFunction_t task,
    const char *name, uint32_t stack_depth, UBaseType_t priority)
{
    worker->stopped = xSemaphoreCreateBinary();
    if (worker->stopped == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(task, name, stack_depth, worker,
                            priority, &worker->task,
                            DISPLAY_BENCHMARK_TASK_CAPS) != pdPASS)
    {
        vSemaphoreDelete(worker->stopped);
        memset(worker, 0, sizeof(*worker));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void _display_benchmark_worker_stop(display_benchmark_worker_t *worker)
{
    if (worker->task != NULL)
    {
        (void)xSemaphoreTake(worker->stopped, portMAX_DELAY);
        vTaskDeleteWithCaps(worker->task);
    }
    if (worker->stopped != NULL)
    {
        vSemaphoreDelete(worker->stopped);
    }
    memset(worker, 0, sizeof(*worker));
}

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
static bool _display_benchmark_is_c_ext_stress(void)
{
    return s_config.ble_mode == DISPLAY_BENCHMARK_BLE_SECURITY2_CONNECTED &&
           s_config.app_workload ==
           DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES;
}

static const char *_display_benchmark_stress_phase_name(
    display_benchmark_stress_phase_t phase)
{
    switch (phase)
    {
    case DISPLAY_BENCHMARK_STRESS_PHASE_BEGIN:
        return "begin";
    case DISPLAY_BENCHMARK_STRESS_PHASE_LOAD_START:
        return "load_start";
    case DISPLAY_BENCHMARK_STRESS_PHASE_PROVISIONING_WAIT:
        return "provisioning_wait";
    case DISPLAY_BENCHMARK_STRESS_PHASE_WARMUP:
        return "warmup";
    case DISPLAY_BENCHMARK_STRESS_PHASE_MEASURE:
        return "measure";
    case DISPLAY_BENCHMARK_STRESS_PHASE_CLEANUP:
        return "cleanup";
    case DISPLAY_BENCHMARK_STRESS_PHASE_END:
        return "end";
    default:
        return "none";
    }
}

static void _display_benchmark_stress_set_phase(
    display_benchmark_stress_context_t *context,
    display_benchmark_stress_phase_t phase)
{
    atomic_store_explicit(&context->phase, phase, memory_order_release);
    LOG_I("c_ext_stress phase=%s sequence=%u",
          _display_benchmark_stress_phase_name(phase), (unsigned)phase);
}

static void _display_benchmark_stress_update_minimum_size(
    size_t *minimum, size_t value, bool sampled)
{
    if (!sampled || value < *minimum)
    {
        *minimum = value;
    }
}

static void _display_benchmark_stress_record_audio_io(
    display_benchmark_stress_context_t *context, bool transmit,
    esp_err_t result, size_t transferred, size_t expected, uint32_t elapsed_us)
{
    atomic_uint *short_count = transmit ?
                               &context->audio_transmit_short_count :
                               &context->audio_receive_short_count;
    atomic_uint *timeout_count = transmit ?
                                 &context->audio_transmit_timeout_count :
                                 &context->audio_receive_timeout_count;
    atomic_uint *error_count = transmit ?
                               &context->audio_transmit_error_count :
                               &context->audio_receive_error_count;
    atomic_uint *deadline_count = transmit ?
                                  &context->audio_transmit_deadline_miss_count :
                                  &context->audio_receive_deadline_miss_count;
    atomic_uint_fast64_t *bytes = transmit ?
                                  &context->audio_transmit_bytes :
                                  &context->audio_receive_bytes;
    atomic_fetch_add_explicit(bytes, transferred, memory_order_relaxed);
    if (transferred != expected)
    {
        atomic_fetch_add_explicit(short_count, 1U, memory_order_relaxed);
    }
    if (result == ESP_ERR_TIMEOUT)
    {
        atomic_fetch_add_explicit(timeout_count, 1U, memory_order_relaxed);
    }
    else if (result != ESP_OK)
    {
        atomic_fetch_add_explicit(error_count, 1U, memory_order_relaxed);
    }
    if (elapsed_us > DISPLAY_BENCHMARK_STRESS_AUDIO_DEADLINE_US)
    {
        atomic_fetch_add_explicit(deadline_count, 1U, memory_order_relaxed);
    }
    if (result != ESP_OK || transferred != expected)
    {
        int expected_error = ESP_OK;
        const esp_err_t audio_error = result == ESP_OK ?
                                      ESP_ERR_INVALID_SIZE : result;
        (void)atomic_compare_exchange_strong_explicit(
            &s_audio_error, &expected_error, audio_error,
            memory_order_relaxed, memory_order_relaxed);
    }
}

static void _display_benchmark_stress_audio_tx_task(void *arg)
{
    display_benchmark_worker_t *worker = arg;
    display_benchmark_stress_context_t *context = worker->context;
    int16_t *buffer = heap_caps_malloc(
                          DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES,
                          DISPLAY_BENCHMARK_TASK_CAPS);
    if (buffer == NULL)
    {
        _display_benchmark_record_error(&s_audio_error, ESP_ERR_NO_MEM);
        goto exit;
    }
    uint32_t phase = 0U;
    while (!_display_benchmark_should_stop())
    {
        for (size_t index = 0U;
                index < DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES /
                sizeof(*buffer); index += 2U)
        {
            const int16_t sample = (phase++ & 0x20U) != 0U ? 256 : -256;
            buffer[index] = sample;
            buffer[index + 1U] = sample;
        }
        size_t written = 0U;
        const int64_t started_us = esp_timer_get_time();
        const esp_err_t result = audio_service_write(
                                     buffer,
                                     DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES,
                                     &written,
                                     DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS);
        const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() -
                                               started_us);
        _display_benchmark_stress_record_audio_io(
            context, true, result, written,
            DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES, elapsed_us);
        if (result == ESP_ERR_INVALID_STATE ||
                audio_service_get_state() != AUDIO_SERVICE_STATE_RUNNING)
        {
            _display_benchmark_record_error(&s_audio_error,
                                            ESP_ERR_INVALID_STATE);
            break;
        }
        if (result != ESP_OK)
        {
            vTaskDelay(1U);
        }
    }

exit:
    heap_caps_free(buffer);
    (void)xSemaphoreGive(worker->stopped);
    vTaskSuspend(NULL);
}

static void _display_benchmark_stress_audio_rx_task(void *arg)
{
    display_benchmark_worker_t *worker = arg;
    display_benchmark_stress_context_t *context = worker->context;
    uint8_t *buffer = heap_caps_malloc(
                          DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES,
                          DISPLAY_BENCHMARK_TASK_CAPS);
    if (buffer == NULL)
    {
        _display_benchmark_record_error(&s_audio_error, ESP_ERR_NO_MEM);
        goto exit;
    }
    while (!_display_benchmark_should_stop())
    {
        size_t captured = 0U;
        const int64_t started_us = esp_timer_get_time();
        const esp_err_t result = audio_service_read(
                                     buffer,
                                     DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES,
                                     &captured,
                                     DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS);
        const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() -
                                               started_us);
        if (result == ESP_OK)
        {
            for (size_t index = 0U; index < captured; ++index)
            {
                if (buffer[index] != 0U)
                {
                    atomic_store_explicit(&context->microphone_nonzero, true,
                                          memory_order_relaxed);
                    break;
                }
            }
        }
        _display_benchmark_stress_record_audio_io(
            context, false, result, captured,
            DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES, elapsed_us);
        memset(buffer, 0, DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES);
        if (result == ESP_ERR_INVALID_STATE ||
                audio_service_get_state() != AUDIO_SERVICE_STATE_RUNNING)
        {
            _display_benchmark_record_error(&s_audio_error,
                                            ESP_ERR_INVALID_STATE);
            break;
        }
        if (result != ESP_OK)
        {
            vTaskDelay(1U);
        }
    }

exit:
    if (buffer != NULL)
    {
        memset(buffer, 0, DISPLAY_BENCHMARK_STRESS_AUDIO_BLOCK_BYTES);
    }
    heap_caps_free(buffer);
    (void)xSemaphoreGive(worker->stopped);
    vTaskSuspend(NULL);
}

static void _display_benchmark_stress_sample_task(
    display_benchmark_stress_context_t *context, size_t index,
    uint32_t *high_water, uint32_t *missing_mask, uint32_t *internal_mask)
{
    display_benchmark_stress_task_metric_t *metric = &context->tasks[index];
    TaskHandle_t task = xTaskGetHandle(metric->name);
    const bool found = task != NULL;
    const uint32_t current_high_water = found ?
                                        (uint32_t)uxTaskGetStackHighWaterMark(task) : 0U;
    const StackType_t *stack_start = found ? xTaskGetStackStart(task) : NULL;
    const bool stack_in_psram = stack_start != NULL &&
                                esp_ptr_external_ram(stack_start);
    if (metric->sample_count == 0U)
    {
        metric->minimum_high_water = current_high_water;
        metric->found_in_every_sample = found;
        metric->stack_in_psram_in_every_sample = stack_in_psram;
    }
    else
    {
        if (current_high_water < metric->minimum_high_water)
        {
            metric->minimum_high_water = current_high_water;
        }
        metric->found_in_every_sample =
            metric->found_in_every_sample && found;
        metric->stack_in_psram_in_every_sample =
            metric->stack_in_psram_in_every_sample && stack_in_psram;
    }
    ++metric->sample_count;
    high_water[index] = current_high_water;
    if (!found)
    {
        *missing_mask |= 1U << index;
    }
    else if (!stack_in_psram)
    {
        *internal_mask |= 1U << index;
    }
}

static void _display_benchmark_stress_observe_ble(
    display_benchmark_stress_context_t *context, int64_t now_us)
{
    provisioning_service_status_t status = {0};
    provisioning_service_diagnostics_t diagnostics = {0};
    const bool connected = provisioning_service_get_status(&status) == ESP_OK &&
                           status.state == PROVISIONING_SERVICE_STATE_CONNECTED &&
                           status.client_connected &&
                           provisioning_service_get_diagnostics(&diagnostics) ==
                           ESP_OK;
    if (!connected && context->ble_was_connected)
    {
        ++context->ble_disconnect_count;
    }
    else if (connected && !context->ble_was_connected)
    {
        ++context->ble_reconnect_count;
    }
    context->ble_was_connected = connected;
    if (!connected || diagnostics.last_snapshot_success_us <= 0)
    {
        return;
    }
    if (context->last_observed_snapshot_success_us > 0 &&
            diagnostics.last_snapshot_success_us >
            context->last_observed_snapshot_success_us)
    {
        const uint64_t interval_us =
            (uint64_t)(diagnostics.last_snapshot_success_us -
                       context->last_observed_snapshot_success_us);
        if (interval_us > context->maximum_ble_success_interval_us)
        {
            context->maximum_ble_success_interval_us = interval_us;
        }
    }
    if (diagnostics.last_snapshot_success_us >
            context->last_observed_snapshot_success_us)
    {
        context->last_observed_snapshot_success_us =
            diagnostics.last_snapshot_success_us;
    }
    if (now_us > diagnostics.last_snapshot_success_us)
    {
        const uint64_t idle_us =
            (uint64_t)(now_us - diagnostics.last_snapshot_success_us);
        if (idle_us > context->maximum_ble_success_idle_us)
        {
            context->maximum_ble_success_idle_us = idle_us;
        }
    }
}

static void _display_benchmark_stress_sampler_task(void *arg)
{
    display_benchmark_worker_t *worker = arg;
    display_benchmark_stress_context_t *context = worker->context;
    while (atomic_load_explicit(&context->sampler_enabled,
                                memory_order_acquire) &&
            !_display_benchmark_should_stop())
    {
        const display_benchmark_stress_phase_t sample_phase =
            (display_benchmark_stress_phase_t)atomic_load_explicit(
                &context->phase, memory_order_acquire);
        if (sample_phase >= DISPLAY_BENCHMARK_STRESS_PHASE_CLEANUP)
        {
            break;
        }
        const int64_t now_us = esp_timer_get_time();
        const size_t internal_free = heap_caps_get_free_size(
                                         DISPLAY_BENCHMARK_STRESS_INTERNAL_CAPS);
        const size_t internal_largest = heap_caps_get_largest_free_block(
                                            DISPLAY_BENCHMARK_STRESS_INTERNAL_CAPS);
        const size_t dma_free = heap_caps_get_free_size(
                                    DISPLAY_BENCHMARK_STRESS_DMA_CAPS);
        const size_t dma_largest = heap_caps_get_largest_free_block(
                                       DISPLAY_BENCHMARK_STRESS_DMA_CAPS);
        const size_t psram_free = heap_caps_get_free_size(
                                      DISPLAY_BENCHMARK_STRESS_PSRAM_CAPS);
        const size_t psram_largest = heap_caps_get_largest_free_block(
                                         DISPLAY_BENCHMARK_STRESS_PSRAM_CAPS);
        _display_benchmark_stress_update_minimum_size(
            &context->minimum_internal_free, internal_free,
            context->heap_sampled);
        _display_benchmark_stress_update_minimum_size(
            &context->minimum_internal_largest, internal_largest,
            context->heap_sampled);
        _display_benchmark_stress_update_minimum_size(
            &context->minimum_dma_free, dma_free, context->heap_sampled);
        _display_benchmark_stress_update_minimum_size(
            &context->minimum_dma_largest, dma_largest,
            context->heap_sampled);
        _display_benchmark_stress_update_minimum_size(
            &context->minimum_psram_free, psram_free, context->heap_sampled);
        _display_benchmark_stress_update_minimum_size(
            &context->minimum_psram_largest, psram_largest,
            context->heap_sampled);
        context->heap_sampled = true;

        uint32_t high_water[DISPLAY_BENCHMARK_STRESS_TASK_COUNT] = {0};
        uint32_t missing_mask = 0U;
        uint32_t internal_mask = 0U;
        for (size_t index = 0U;
                index < DISPLAY_BENCHMARK_STRESS_TASK_COUNT; ++index)
        {
            _display_benchmark_stress_sample_task(
                context, index, high_water, &missing_mask, &internal_mask);
        }
        _display_benchmark_stress_observe_ble(context, now_us);
        const int64_t measure_start_us = atomic_load_explicit(
                                             &context->measure_start_us,
                                             memory_order_acquire);
        if (measure_start_us > 0 && now_us >= measure_start_us &&
                context->heap_sample_count < context->heap_sample_capacity)
        {
            display_benchmark_stress_heap_sample_t *sample =
                &context->heap_samples[context->heap_sample_count++];
            sample->elapsed_us = now_us - measure_start_us;
            sample->psram_free = psram_free;
        }
        LOG_I("c_ext_stress sample=%u phase=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u task_missing=0x%x task_internal=0x%x hwm_lvgl=%u hwm_connectivity=%u hwm_wifi=%u hwm_provisioning=%u hwm_nimble=%u hwm_supervisor=%u hwm_tcp=%u hwm_audio_tx=%u hwm_audio_rx=%u hwm_sampler=%u",
              (unsigned)context->tasks[0].sample_count,
              _display_benchmark_stress_phase_name(sample_phase),
              (unsigned)internal_free, (unsigned)internal_largest,
              (unsigned)dma_free, (unsigned)dma_largest,
              (unsigned)psram_free, (unsigned)psram_largest,
              (unsigned)missing_mask, (unsigned)internal_mask,
              (unsigned)high_water[0], (unsigned)high_water[1],
              (unsigned)high_water[2], (unsigned)high_water[3],
              (unsigned)high_water[4], (unsigned)high_water[5],
              (unsigned)high_water[6], (unsigned)high_water[7],
              (unsigned)high_water[8], (unsigned)high_water[9]);
        if (internal_free == 0U || internal_largest == 0U || dma_free == 0U ||
                dma_largest == 0U || psram_free == 0U || psram_largest == 0U)
        {
            _display_benchmark_record_error(&s_control_error, ESP_ERR_NO_MEM);
            break;
        }
        if (audio_service_get_state() != AUDIO_SERVICE_STATE_RUNNING)
        {
            _display_benchmark_record_error(&s_audio_error,
                                            ESP_ERR_INVALID_STATE);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_BENCHMARK_STRESS_SAMPLE_MS));
    }
    (void)xSemaphoreGive(worker->stopped);
    vTaskSuspend(NULL);
}
#endif

static esp_err_t _display_benchmark_submit_navigation(
    const app_manager_nav_request_t *request)
{
    esp_err_t result = app_manager_pm_notify_user_activity();
    if (result == ESP_OK)
    {
        result = app_manager_navigate(
                     request, APP_MANAGER_TRANSITION_MAX_DURATION_MS + 1000U);
    }
    if (result == ESP_OK)
    {
        result = app_manager_display_diagnostics_wait_for_presentation(
                     DISPLAY_BENCHMARK_PRESENTATION_TIMEOUT_MS);
    }
    return result;
}

static esp_err_t _display_benchmark_navigate_effect(
    display_benchmark_page_t *page, app_manager_transition_effect_t effect)
{
    const bool show_settings = *page == DISPLAY_BENCHMARK_PAGE_HOME;
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = show_settings ? APP_MANAGER_ID_SETTINGS :
        APP_MANAGER_ID_HOME,
        .transition =
        {
            .effect = effect,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
    };
    const esp_err_t result = _display_benchmark_submit_navigation(&request);
    if (result == ESP_OK)
    {
        *page = show_settings ? DISPLAY_BENCHMARK_PAGE_SETTINGS :
                DISPLAY_BENCHMARK_PAGE_HOME;
    }
    return result;
}

static esp_err_t _display_benchmark_navigate_stress(
    display_benchmark_page_t *page, int64_t elapsed_us)
{
    app_manager_nav_request_t request =
    {
        .transition =
        {
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
    };
    const bool fade_phase =
        (elapsed_us / DISPLAY_BENCHMARK_PHASE_US) % 2LL == 0LL;
    if (fade_phase)
    {
        if (*page == DISPLAY_BENCHMARK_PAGE_ABOUT)
        {
            request.operation = APP_MANAGER_NAV_OP_BACK;
            request.transition.effect = APP_MANAGER_TRANSITION_PUSH_RIGHT;
            *page = DISPLAY_BENCHMARK_PAGE_SETTINGS;
        }
        else
        {
            request.operation = APP_MANAGER_NAV_OP_RUN;
            request.app_id = *page == DISPLAY_BENCHMARK_PAGE_HOME ?
                             APP_MANAGER_ID_SETTINGS : APP_MANAGER_ID_HOME;
            request.transition.effect = APP_MANAGER_TRANSITION_FADE;
            *page = *page == DISPLAY_BENCHMARK_PAGE_HOME ?
                    DISPLAY_BENCHMARK_PAGE_SETTINGS :
                    DISPLAY_BENCHMARK_PAGE_HOME;
        }
    }
    else if (*page == DISPLAY_BENCHMARK_PAGE_HOME)
    {
        request.operation = APP_MANAGER_NAV_OP_RUN;
        request.app_id = APP_MANAGER_ID_SETTINGS;
        request.transition.effect = APP_MANAGER_TRANSITION_FADE;
        *page = DISPLAY_BENCHMARK_PAGE_SETTINGS;
    }
    else if (*page == DISPLAY_BENCHMARK_PAGE_SETTINGS)
    {
        request.operation = APP_MANAGER_NAV_OP_OPEN_PAGE;
        request.app_id = APP_MANAGER_ID_SETTINGS;
        request.page_id = "about";
        request.transition.effect = APP_MANAGER_TRANSITION_PUSH_LEFT;
        *page = DISPLAY_BENCHMARK_PAGE_ABOUT;
    }
    else
    {
        request.operation = APP_MANAGER_NAV_OP_BACK;
        request.transition.effect = APP_MANAGER_TRANSITION_PUSH_RIGHT;
        *page = DISPLAY_BENCHMARK_PAGE_SETTINGS;
    }

    return _display_benchmark_submit_navigation(&request);
}

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
static esp_err_t _display_benchmark_stress_navigate(
    const char *app_id, const char *page_id,
    app_manager_transition_effect_t effect)
{
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = app_id,
        .page_id = page_id,
        .transition =
        {
            .effect = effect,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
    };
    return _display_benchmark_submit_navigation(&request);
}

static esp_err_t _display_benchmark_stress_cleanup_background(void)
{
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND,
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_NONE,
            .duration_ms = 0U,
        },
    };
    return _display_benchmark_submit_navigation(&request);
}

static esp_err_t _display_benchmark_stress_open_provisioning(void)
{
    esp_err_t result = _display_benchmark_stress_navigate(
                           APP_MANAGER_ID_SETUP, "root",
                           APP_MANAGER_TRANSITION_FADE);
    if (result == ESP_OK)
    {
        result = _display_benchmark_stress_navigate(
                     APP_MANAGER_ID_SETUP, "provisioning",
                     APP_MANAGER_TRANSITION_PUSH_LEFT);
    }
    return result;
}

static esp_err_t _display_benchmark_stress_wait_for_provisioning(
    display_benchmark_stress_context_t *context)
{
    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)DISPLAY_BENCHMARK_STRESS_PROVISIONING_MS *
                                1000LL;
    while (!_display_benchmark_should_stop() &&
            esp_timer_get_time() < deadline_us)
    {
        provisioning_service_status_t status = {0};
        provisioning_service_diagnostics_t diagnostics = {0};
        const esp_err_t status_result = provisioning_service_get_status(&status);
        const esp_err_t diagnostics_result =
            provisioning_service_get_diagnostics(&diagnostics);
        if (status_result == ESP_OK && diagnostics_result == ESP_OK &&
                status.state == PROVISIONING_SERVICE_STATE_CONNECTED &&
                status.client_connected &&
                diagnostics.snapshot_success_count > 0U &&
                diagnostics.last_snapshot_request_id != 0U &&
                diagnostics.last_snapshot_success_us > 0)
        {
            context->ble_was_connected = true;
            context->last_observed_snapshot_success_us =
                diagnostics.last_snapshot_success_us;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_BENCHMARK_STRESS_SAMPLE_MS));
    }
    return _display_benchmark_should_stop() ? ESP_ERR_INVALID_STATE :
           ESP_ERR_TIMEOUT;
}

static esp_err_t _display_benchmark_stress_warmup(
    display_benchmark_page_t *page)
{
    esp_err_t result = _display_benchmark_stress_navigate(
                           APP_MANAGER_ID_HOME, "root",
                           APP_MANAGER_TRANSITION_FADE);
    if (result != ESP_OK)
    {
        return result;
    }
    *page = DISPLAY_BENCHMARK_PAGE_HOME;
    for (size_t effect_index = 0U;
            effect_index < DISPLAY_BENCHMARK_STRESS_EFFECT_COUNT;
            ++effect_index)
    {
        for (size_t repetition = 0U;
                repetition < DISPLAY_BENCHMARK_STRESS_WARMUP_REPETITIONS;
                ++repetition)
        {
            result = _display_benchmark_navigate_effect(
                         page, s_stress_effects[effect_index]);
            if (result != ESP_OK || _display_benchmark_should_stop())
            {
                return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t _display_benchmark_stress_run_routes(
    display_benchmark_stress_context_t *context, int64_t started_us,
    bool *completed)
{
    esp_err_t result = ESP_OK;
    size_t route_index = 0U;
    size_t effect_index = 0U;
    const int64_t duration_us = _display_benchmark_stress_duration_us();
    while (!_display_benchmark_should_stop() &&
            atomic_load_explicit(&s_workload_error,
                                 memory_order_relaxed) == ESP_OK)
    {
        if (esp_timer_get_time() - started_us >= duration_us)
        {
            break;
        }
        _display_benchmark_check_wifi();
        const display_benchmark_stress_route_t *route =
            &s_stress_routes[route_index];
        result = _display_benchmark_stress_navigate(
                     route->app_id, route->page_id,
                     s_stress_effects[effect_index]);
        if (result != ESP_OK)
        {
            break;
        }
        context->route_visit_mask |= 1U << route_index;
        context->effect_visit_mask |= 1U << effect_index;
        vTaskDelay(pdMS_TO_TICKS(route->dwell_ms));
        route_index = (route_index + 1U) %
                      DISPLAY_BENCHMARK_STRESS_ROUTE_COUNT;
        effect_index = (effect_index + 1U) %
                       DISPLAY_BENCHMARK_STRESS_EFFECT_COUNT;
    }
    *completed = !_display_benchmark_should_stop() &&
                 esp_timer_get_time() - started_us >= duration_us;
    return result;
}
#endif

static bool _display_benchmark_wifi_ready(void)
{
    connectivity_manager_status_snapshot_t status = {0};
    return connectivity_manager_get_status(&status) == ESP_OK &&
           status.state == CONNECTIVITY_MANAGER_STATE_IP_READY;
}

static const char *_display_benchmark_load_name(display_benchmark_load_t load)
{
    switch (load)
    {
    case DISPLAY_BENCHMARK_LOAD_FULL:
        return "full";
    case DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY:
        return "audio-only";
    case DISPLAY_BENCHMARK_LOAD_TCP_ONLY:
        return "tcp-only";
    default:
        return "display-only";
    }
}

static bool _display_benchmark_load_requires_audio(
    display_benchmark_load_t load)
{
    return load == DISPLAY_BENCHMARK_LOAD_FULL ||
           load == DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY;
}

static bool _display_benchmark_load_requires_tcp(
    display_benchmark_load_t load)
{
    return load == DISPLAY_BENCHMARK_LOAD_FULL ||
           load == DISPLAY_BENCHMARK_LOAD_TCP_ONLY;
}

static void _display_benchmark_observe_wifi(bool ready)
{
    if (!atomic_load_explicit(&s_wifi_monitoring_active,
                              memory_order_acquire))
    {
        return;
    }
    const bool was_ready = atomic_exchange_explicit(&s_wifi_was_ready, ready,
                           memory_order_relaxed);
    if (!ready && was_ready)
    {
        atomic_fetch_add_explicit(&s_wifi_disconnect_count, 1U,
                                  memory_order_relaxed);
        _display_benchmark_record_stability_error(&s_control_error, ESP_FAIL);
    }
}

static void _display_benchmark_wifi_event(event_bus_msg_id_t msg_id,
        uint32_t sub_type, const void *payload, size_t payload_size,
        void *user_data)
{
    (void)user_data;
    if (msg_id != CONNECTIVITY_MANAGER_MSG ||
            sub_type !=
            CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT ||
            payload == NULL ||
            payload_size != sizeof(connectivity_manager_status_snapshot_t))
    {
        return;
    }
    connectivity_manager_status_snapshot_t status;
    memcpy(&status, payload, sizeof(status));
    _display_benchmark_observe_wifi(
        status.state == CONNECTIVITY_MANAGER_STATE_IP_READY);
}

static display_benchmark_tcp_report_t _display_benchmark_tcp_finish_report(
    uint64_t active_duration_us, bool required)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t disconnected_since_us = atomic_exchange_explicit(
            &s_tcp_disconnected_since_us, 0, memory_order_relaxed);
    if (disconnected_since_us > 0 && now_us > disconnected_since_us)
    {
        atomic_fetch_add_explicit(
            &s_tcp_interruption_us,
            (uint64_t)(now_us - disconnected_since_us), memory_order_relaxed);
    }

    display_benchmark_tcp_report_t report =
    {
        .transmit_bytes = atomic_load_explicit(&s_tcp_transmit_bytes,
                                               memory_order_relaxed),
        .receive_bytes = atomic_load_explicit(&s_tcp_receive_bytes,
                                              memory_order_relaxed),
        .target_bytes =
        (uint64_t)s_config.rate_kbit_s *
        active_duration_us / 8000U,
        .active_duration_us = active_duration_us,
        .interruption_us = atomic_load_explicit(&s_tcp_interruption_us,
                                                memory_order_relaxed),
        .maximum_pacing_lag_us = atomic_load_explicit(
            &s_tcp_maximum_pacing_lag_us,
            memory_order_relaxed),
        .reconnect_count = atomic_load_explicit(&s_tcp_reconnect_count,
                                                memory_order_relaxed),
        .pacing_late_count = atomic_load_explicit(&s_tcp_pacing_late_count,
            memory_order_relaxed),
        .required = required,
    };
    const uint64_t minimum_bytes =
        (report.target_bytes * DISPLAY_BENCHMARK_TCP_MINIMUM_PERCENT + 99U) /
        100U;
    report.throughput_passed = !required ||
                               (report.active_duration_us > 0U &&
                                report.transmit_bytes >= minimum_bytes &&
                                report.receive_bytes >= minimum_bytes);
    return report;
}

static const char *_display_benchmark_effect_name(
    app_manager_transition_effect_t effect)
{
    switch (effect)
    {
    case APP_MANAGER_TRANSITION_FADE:
        return "fade";
    case APP_MANAGER_TRANSITION_COVER_LEFT:
        return "cover-left";
    case APP_MANAGER_TRANSITION_PUSH_LEFT:
        return "push-left";
    case APP_MANAGER_TRANSITION_PUSH_RIGHT:
        return "push-right";
    case APP_MANAGER_TRANSITION_REVEAL_RIGHT:
        return "reveal-right";
    default:
        return "other";
    }
}

static const char *_display_benchmark_performance_name(
    display_benchmark_performance_t performance)
{
    switch (performance)
    {
    case DISPLAY_BENCHMARK_PERFORMANCE_TARGET:
        return "TARGET";
    case DISPLAY_BENCHMARK_PERFORMANCE_FLOOR:
        return "FLOOR";
    default:
        return "FAIL";
    }
}

static display_benchmark_performance_t _display_benchmark_worst_performance(
    display_benchmark_performance_t left,
    display_benchmark_performance_t right)
{
    return left < right ? left : right;
}

static uint32_t _display_benchmark_average_fps_x100(
    const app_manager_display_effect_benchmark_report_t *effect)
{
    if (effect->active_duration_us == 0U)
    {
        return 0U;
    }
    const uint64_t frames_x100 =
        (uint64_t)effect->active_frame_count * 100000000ULL;
    const uint64_t average = frames_x100 / effect->active_duration_us;
    return average > UINT32_MAX ? UINT32_MAX : (uint32_t)average;
}

static bool _display_benchmark_snapshot_prepare_passed(
    const app_manager_display_effect_benchmark_report_t *effect)
{
#if DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_ENABLED
    return effect->snapshot_prepare_count == effect->transition_start_count &&
           effect->snapshot_prepare_count > 0U &&
           effect->snapshot_prepare_p95_us <=
           APP_MANAGER_DISPLAY_SNAPSHOT_PREPARE_TARGET_US &&
           effect->snapshot_fallback_count == 0U;
#else
    (void)effect;
    return true;
#endif
}

static display_benchmark_performance_t _display_benchmark_grade_effect(
    const app_manager_display_effect_benchmark_report_t *effect)
{
    if (effect->transition_start_count == 0U ||
            effect->transition_complete_count !=
            effect->transition_start_count ||
            effect->transition_cancel_count != 0U ||
            effect->active_frame_count == 0U ||
            effect->active_duration_us == 0U ||
            effect->interval_count == 0U ||
            !_display_benchmark_snapshot_prepare_passed(effect))
    {
        return DISPLAY_BENCHMARK_PERFORMANCE_FAIL;
    }

    const bool floor_passed =
        (uint64_t)effect->active_frame_count * 1000000ULL >=
        (uint64_t)DISPLAY_BENCHMARK_FLOOR_FPS *
        effect->active_duration_us &&
        effect->interval_p95_us <= DISPLAY_BENCHMARK_FLOOR_P95_US &&
        effect->interval_p99_us <= DISPLAY_BENCHMARK_FLOOR_P99_US &&
        effect->maximum_interval_us <= DISPLAY_BENCHMARK_FLOOR_MAX_US;
    if (!floor_passed)
    {
        return DISPLAY_BENCHMARK_PERFORMANCE_FAIL;
    }

    const bool target_passed =
        (uint64_t)effect->active_frame_count * 1000000ULL >=
        (uint64_t)DISPLAY_BENCHMARK_TARGET_FPS *
        effect->active_duration_us &&
        (uint64_t)effect->interval_within_target_count * 100ULL >=
        (uint64_t)effect->interval_count *
        DISPLAY_BENCHMARK_TARGET_PERCENT &&
        effect->interval_p99_us <= APP_MANAGER_DISPLAY_INTERVAL_LONG_US &&
        effect->maximum_consecutive_long_intervals <= 1U;
    return target_passed ? DISPLAY_BENCHMARK_PERFORMANCE_TARGET :
           DISPLAY_BENCHMARK_PERFORMANCE_FLOOR;
}

static bool _display_benchmark_snapshot_prepare_all_passed(
    const app_manager_display_benchmark_report_t *report)
{
#if DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_ENABLED
    if (report->snapshot_fallback_count != 0U)
    {
        return false;
    }
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT; ++index)
    {
        const app_manager_display_effect_benchmark_report_t *effect =
            &report->effects[index];
        if (effect->transition_start_count == 0U)
        {
            continue;
        }
        if (!_display_benchmark_snapshot_prepare_passed(effect))
        {
            return false;
        }
    }
#else
    (void)report;
#endif
    return true;
}

static display_benchmark_performance_t
_display_benchmark_grade_production_effects(
    const app_manager_display_benchmark_report_t *report)
{
    static const app_manager_transition_effect_t production_effects[] =
    {
        APP_MANAGER_TRANSITION_FADE,
        APP_MANAGER_TRANSITION_PUSH_LEFT,
        APP_MANAGER_TRANSITION_PUSH_RIGHT,
    };
    display_benchmark_performance_t performance =
        DISPLAY_BENCHMARK_PERFORMANCE_TARGET;
    for (size_t index = 0U;
            index < sizeof(production_effects) / sizeof(production_effects[0]);
            ++index)
    {
        const size_t report_index =
            (size_t)(production_effects[index] - APP_MANAGER_TRANSITION_NONE);
        performance = _display_benchmark_worst_performance(
                          performance,
                          _display_benchmark_grade_effect(
                              &report->effects[report_index]));
    }
    return performance;
}

static display_benchmark_performance_t _display_benchmark_grade_profile(
    const app_manager_display_benchmark_report_t *report)
{
    const display_benchmark_performance_t performance =
        _display_benchmark_grade_production_effects(report);
    return _display_benchmark_snapshot_prepare_all_passed(report) ?
           performance : DISPLAY_BENCHMARK_PERFORMANCE_FAIL;
}

static void _display_benchmark_accumulate_stability(
    display_benchmark_stability_summary_t *summary,
    const app_manager_display_benchmark_report_t *report)
{
    const bool first_report = summary->report_count == 0U;
    ++summary->report_count;
    summary->sample_count += report->sample_count;
    summary->sample_error_count += report->sample_error_count;
    summary->lvgl_lock_error_count += report->lvgl_lock_error_count;
    summary->fps_read_error_count += report->fps_read_error_count;
    if (report->maximum_fps_lock_wait_us >
            summary->maximum_fps_lock_wait_us)
    {
        summary->maximum_fps_lock_wait_us =
            report->maximum_fps_lock_wait_us;
    }
    if (report->sample_count > 0U)
    {
        if (!summary->heap_sampled)
        {
            summary->minimum_internal_free = report->minimum_internal_free;
            summary->minimum_internal_largest =
                report->minimum_internal_largest;
            summary->minimum_dma_free = report->minimum_dma_free;
            summary->minimum_dma_largest = report->minimum_dma_largest;
            summary->minimum_psram_free = report->minimum_psram_free;
            summary->minimum_psram_largest = report->minimum_psram_largest;
            summary->heap_sampled = true;
        }
        else
        {
            if (report->minimum_internal_free < summary->minimum_internal_free)
            {
                summary->minimum_internal_free = report->minimum_internal_free;
            }
            if (report->minimum_internal_largest <
                    summary->minimum_internal_largest)
            {
                summary->minimum_internal_largest =
                    report->minimum_internal_largest;
            }
            if (report->minimum_dma_free < summary->minimum_dma_free)
            {
                summary->minimum_dma_free = report->minimum_dma_free;
            }
            if (report->minimum_dma_largest < summary->minimum_dma_largest)
            {
                summary->minimum_dma_largest = report->minimum_dma_largest;
            }
            if (report->minimum_psram_free < summary->minimum_psram_free)
            {
                summary->minimum_psram_free = report->minimum_psram_free;
            }
            if (report->minimum_psram_largest < summary->minimum_psram_largest)
            {
                summary->minimum_psram_largest =
                    report->minimum_psram_largest;
            }
        }
    }
#if CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT == 1
    summary->render_task_found = first_report ? report->render_task_found :
                                 summary->render_task_found &&
                                 report->render_task_found;
    summary->render_task_stack_in_psram = first_report ?
                                          report->render_task_stack_in_psram :
                                          summary->render_task_stack_in_psram &&
                                          report->render_task_stack_in_psram;
    if (first_report || report->minimum_render_stack_high_water <
            summary->minimum_render_stack_high_water)
    {
        summary->minimum_render_stack_high_water =
            report->minimum_render_stack_high_water;
    }
#else
    (void)first_report;
#endif
    summary->dma_failure_count += report->dma_failure_count;
    summary->frame_submit_count += report->frame_submit_count;
    summary->submit_failure_count += report->submit_failure_count;
    summary->transition_cancel_count += report->transition_cancel_count;
    summary->snapshot_prepare_count += report->snapshot_prepare_count;
    summary->snapshot_prepare_us += report->snapshot_prepare_us;
    if (report->maximum_snapshot_prepare_us >
            summary->maximum_snapshot_prepare_us)
    {
        summary->maximum_snapshot_prepare_us =
            report->maximum_snapshot_prepare_us;
    }
    if (report->snapshot_prepare_p95_us >
            summary->maximum_snapshot_prepare_p95_us)
    {
        summary->maximum_snapshot_prepare_p95_us =
            report->snapshot_prepare_p95_us;
    }
    summary->snapshot_fallback_count += report->snapshot_fallback_count;
    summary->diagnostics_passed = summary->diagnostics_passed &&
                                  report->passed;
}

static void _display_benchmark_log_profile(
    display_benchmark_load_t load,
    const app_manager_display_benchmark_report_t *report)
{
    const char *load_name = _display_benchmark_load_name(load);
    for (size_t index = 0U;
            index < APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_COUNT; ++index)
    {
        const app_manager_display_effect_benchmark_report_t *effect =
            &report->effects[index];
        if (effect->transition_start_count == 0U)
        {
            continue;
        }
        const uint32_t target_percent_x100 = effect->interval_count == 0U ?
                                             0U :
                                             (uint32_t)(
                                                     (uint64_t)effect->interval_within_target_count *
                                                     10000ULL /
                                                     effect->interval_count);
        LOG_I("display perf load=%s effect=%s result=%s start=%u complete=%u cancel=%u snapshot=%s snapshot_prepare_count=%u snapshot_prepare_us=%llu snapshot_prepare_max_us=%u snapshot_prepare_p95_us=%u snapshot_fallbacks=%u active_frames=%u active_ms=%llu avg_fps_x100=%u intervals=%u target_pct_x100=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u long_run=%u",
              load_name, _display_benchmark_effect_name(effect->effect),
              _display_benchmark_performance_name(
                  _display_benchmark_grade_effect(effect)),
              (unsigned)effect->transition_start_count,
              (unsigned)effect->transition_complete_count,
              (unsigned)effect->transition_cancel_count,
              DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_NAME,
              (unsigned)effect->snapshot_prepare_count,
              (unsigned long long)effect->snapshot_prepare_us,
              (unsigned)effect->maximum_snapshot_prepare_us,
              (unsigned)effect->snapshot_prepare_p95_us,
              (unsigned)effect->snapshot_fallback_count,
              (unsigned)effect->active_frame_count,
              (unsigned long long)(effect->active_duration_us / 1000U),
              (unsigned)_display_benchmark_average_fps_x100(effect),
              (unsigned)effect->interval_count,
              (unsigned)target_percent_x100,
              (unsigned)effect->interval_p50_us,
              (unsigned)effect->interval_p95_us,
              (unsigned)effect->interval_p99_us,
              (unsigned)effect->maximum_interval_us,
              (unsigned)effect->maximum_consecutive_long_intervals);
        LOG_I("display cost load=%s effect=%s render_count=%u render_us=%llu render_max_us=%u flush_count=%u flush_pixels=%llu flush_us=%llu flush_max_us=%u flush_wait_count=%u flush_wait_us=%llu flush_wait_max_us=%u panel_count=%u panel_pixels=%llu panel_us=%llu panel_max_us=%u",
              load_name, _display_benchmark_effect_name(effect->effect),
              (unsigned)effect->render_count,
              (unsigned long long)effect->render_us,
              (unsigned)effect->maximum_render_us,
              (unsigned)effect->flush_count,
              (unsigned long long)effect->flush_pixel_count,
              (unsigned long long)effect->flush_us,
              (unsigned)effect->maximum_flush_us,
              (unsigned)effect->flush_wait_count,
              (unsigned long long)effect->flush_wait_us,
              (unsigned)effect->maximum_flush_wait_us,
              (unsigned)effect->panel_submit_count,
              (unsigned long long)effect->panel_pixel_count,
              (unsigned long long)effect->submit_wait_us,
              (unsigned)effect->maximum_submit_wait_us);
    }
    LOG_I("display profile load=%s diagnostics=%s snapshot=%s snapshot_prepare_count=%u snapshot_prepare_us=%llu snapshot_prepare_max_us=%u snapshot_prepare_p95_us=%u snapshot_fallbacks=%u samples=%u sample_err=%u lock_err=%u fps_read_err=%u fps_lock_max_us=%u min_fps=%u fps_below_30=%u min_dma=%u dma_fail=%u frame_submits=%u panel_submits=%u submit_fail=%u",
          load_name, report->passed ? "PASS" : "FAIL",
          DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_NAME,
          (unsigned)report->snapshot_prepare_count,
          (unsigned long long)report->snapshot_prepare_us,
          (unsigned)report->maximum_snapshot_prepare_us,
          (unsigned)report->snapshot_prepare_p95_us,
          (unsigned)report->snapshot_fallback_count,
          (unsigned)report->sample_count,
          (unsigned)report->sample_error_count,
          (unsigned)report->lvgl_lock_error_count,
          (unsigned)report->fps_read_error_count,
          (unsigned)report->maximum_fps_lock_wait_us,
          (unsigned)report->minimum_fps,
          (unsigned)report->fps_failure_count,
          (unsigned)report->minimum_dma_largest,
          (unsigned)report->dma_failure_count,
          (unsigned)report->frame_submit_count,
          (unsigned)report->panel_submit_count,
          (unsigned)report->submit_failure_count);
    LOG_I("display memory load=%s min_internal_free=%u min_internal_largest=%u min_dma_free=%u min_dma_largest=%u min_psram_free=%u min_psram_largest=%u render_task=%u render_stack_psram=%u render_stack_hwm=%u",
          load_name,
          (unsigned)report->minimum_internal_free,
          (unsigned)report->minimum_internal_largest,
          (unsigned)report->minimum_dma_free,
          (unsigned)report->minimum_dma_largest,
          (unsigned)report->minimum_psram_free,
          (unsigned)report->minimum_psram_largest,
          report->render_task_found ? 1U : 0U,
          report->render_task_stack_in_psram ? 1U : 0U,
          (unsigned)report->minimum_render_stack_high_water);
}

static void _display_benchmark_log_final(
    const display_benchmark_stability_summary_t *summary,
    const display_benchmark_tcp_report_t *tcp_report,
    display_benchmark_load_t load, display_benchmark_performance_t performance,
    bool completed, bool aborted)
{
    const esp_err_t workload_error = atomic_load_explicit(&s_workload_error,
                                     memory_order_relaxed);
    const esp_err_t control_error = atomic_load_explicit(&s_control_error,
                                    memory_order_relaxed);
    const esp_err_t audio_error = atomic_load_explicit(&s_audio_error,
                                  memory_order_relaxed);
    const esp_err_t tcp_error = atomic_load_explicit(&s_tcp_error,
                                memory_order_relaxed);
    const bool heap_passed = summary->heap_sampled &&
                             summary->minimum_internal_free > 0U &&
                             summary->minimum_internal_largest > 0U &&
                             summary->minimum_dma_free > 0U &&
                             summary->minimum_dma_largest > 0U &&
                             summary->minimum_psram_free > 0U &&
                             summary->minimum_psram_largest > 0U;
#if CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT == 1
    bool render_passed = summary->render_task_found &&
                         summary->minimum_render_stack_high_water >=
                         DISPLAY_BENCHMARK_RENDER_MINIMUM_HWM;
#if defined(CONFIG_LV_OS_NONE) && CONFIG_LV_OS_NONE
    render_passed = render_passed && summary->render_task_stack_in_psram;
#endif
#else
    const bool render_passed = true;
#endif
    const bool stability_passed = !aborted && completed && heap_passed &&
                                  render_passed &&
                                  workload_error == ESP_OK &&
                                  control_error == ESP_OK &&
                                  audio_error == ESP_OK && tcp_error == ESP_OK &&
                                  atomic_load_explicit(
                                      &s_wifi_disconnect_count,
                                      memory_order_relaxed) == 0U &&
                                  tcp_report->reconnect_count == 0U &&
                                  tcp_report->throughput_passed &&
                                  summary->diagnostics_passed;
    LOG_I("display benchmark stability=%s performance=%s state=%s snapshot=%s snapshot_prepare_count=%u snapshot_prepare_us=%llu snapshot_prepare_max_us=%u snapshot_prepare_max_p95_us=%u snapshot_fallbacks=%u samples=%u sample_err=%u lock_err=%u fps_read_err=%u fps_lock_max_us=%u min_dma=%u dma_fail=%u frame_submits=%u submit_fail=%u transition_cancel=%u",
          stability_passed ? "PASS" : "FAIL",
          _display_benchmark_performance_name(performance),
          aborted ? "ABORT" : (completed ? "COMPLETE" : "INCOMPLETE"),
          DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_NAME,
          (unsigned)summary->snapshot_prepare_count,
          (unsigned long long)summary->snapshot_prepare_us,
          (unsigned)summary->maximum_snapshot_prepare_us,
          (unsigned)summary->maximum_snapshot_prepare_p95_us,
          (unsigned)summary->snapshot_fallback_count,
          (unsigned)summary->sample_count,
          (unsigned)summary->sample_error_count,
          (unsigned)summary->lvgl_lock_error_count,
          (unsigned)summary->fps_read_error_count,
          (unsigned)summary->maximum_fps_lock_wait_us,
          (unsigned)summary->minimum_dma_largest,
          (unsigned)summary->dma_failure_count,
          (unsigned)summary->frame_submit_count,
          (unsigned)summary->submit_failure_count,
          (unsigned)summary->transition_cancel_count);
    LOG_I("display memory summary min_internal_free=%u min_internal_largest=%u min_dma_free=%u min_dma_largest=%u min_psram_free=%u min_psram_largest=%u render_task=%u render_stack_psram=%u render_stack_hwm=%u",
          (unsigned)summary->minimum_internal_free,
          (unsigned)summary->minimum_internal_largest,
          (unsigned)summary->minimum_dma_free,
          (unsigned)summary->minimum_dma_largest,
          (unsigned)summary->minimum_psram_free,
          (unsigned)summary->minimum_psram_largest,
          summary->render_task_found ? 1U : 0U,
          summary->render_task_stack_in_psram ? 1U : 0U,
          (unsigned)summary->minimum_render_stack_high_water);
    LOG_I("display load profile=%s tcp_required=%u tcp_tx_bytes=%llu tcp_rx_bytes=%llu tcp_target_bytes=%llu tcp_active_us=%llu tcp_rate_ok=%u tcp_reconnects=%u tcp_down_ms=%llu tcp_pacing_late=%u tcp_pacing_max_lag_us=%llu wifi_disconnects=%u workload=0x%x control=0x%x audio=0x%x tcp=0x%x",
          _display_benchmark_load_name(load),
          tcp_report->required ? 1U : 0U,
          (unsigned long long)tcp_report->transmit_bytes,
          (unsigned long long)tcp_report->receive_bytes,
          (unsigned long long)tcp_report->target_bytes,
          (unsigned long long)tcp_report->active_duration_us,
          tcp_report->throughput_passed ? 1U : 0U,
          (unsigned)tcp_report->reconnect_count,
          (unsigned long long)(tcp_report->interruption_us / 1000U),
          (unsigned)tcp_report->pacing_late_count,
          (unsigned long long)tcp_report->maximum_pacing_lag_us,
          (unsigned)atomic_load_explicit(&s_wifi_disconnect_count,
                                         memory_order_relaxed),
          (unsigned)workload_error, (unsigned)control_error,
          (unsigned)audio_error, (unsigned)tcp_error);
}

static void _display_benchmark_log_config(void)
{
    LOG_I("display config qspi_hz=%u draw_rows=%u color=%s snapshot=%s dma_rows=%u dma_max_full_rows=%u queue=%u direct=%u te=%u lv_os=%s draw_units=%u draw_stack=%u draw_prio=%u freetype_pool=%u adapter_stack=%u tcp_payload=%u tcp_prio=%u load_profile=%s lifecycle_log=%u",
          (unsigned)CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ,
          (unsigned)CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT,
          DISPLAY_BENCHMARK_COLOR_FORMAT,
          DISPLAY_BENCHMARK_SNAPSHOT_ANIMATION_NAME,
          (unsigned)CONFIG_BSP_DISPLAY_SPI_MAX_TRANSFER_LINES,
          (unsigned)DISPLAY_BENCHMARK_DMA_MAX_FULL_LINES,
          (unsigned)CONFIG_BSP_DISPLAY_SPI_TRANS_QUEUE_DEPTH,
          (unsigned)DISPLAY_BENCHMARK_DIRECT_DMA_ENABLED,
          (unsigned)DISPLAY_BENCHMARK_TE_ENABLED,
          DISPLAY_BENCHMARK_LVGL_OS_NAME,
          (unsigned)CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT,
          (unsigned)DISPLAY_BENCHMARK_DRAW_STACK_SIZE,
          (unsigned)DISPLAY_BENCHMARK_DRAW_PRIORITY,
          (unsigned)DISPLAY_BENCHMARK_FREETYPE_RENDER_POOL_SIZE,
          (unsigned)CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE,
          (unsigned)DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES,
          (unsigned)DISPLAY_BENCHMARK_TCP_PRIORITY,
          _display_benchmark_load_name(s_config.load),
          (unsigned)DISPLAY_BENCHMARK_LIFECYCLE_LOG_ENABLED);
}

static void _display_benchmark_record_stability_error(atomic_int *source,
        esp_err_t error)
{
    int expected = ESP_OK;
    if (error != ESP_OK)
    {
        (void)atomic_compare_exchange_strong_explicit(source, &expected,
                error, memory_order_relaxed, memory_order_relaxed);
    }
}

static void _display_benchmark_check_wifi(void)
{
    if (_display_benchmark_load_requires_tcp(
                s_config.load))
    {
        _display_benchmark_observe_wifi(_display_benchmark_wifi_ready());
    }
}

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
static esp_err_t _display_benchmark_stress_wait_for_tcp(void)
{
    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)DISPLAY_BENCHMARK_STRESS_TCP_CONNECT_MS *
                                1000LL;
    while (!_display_benchmark_should_stop() &&
            esp_timer_get_time() < deadline_us)
    {
        if (atomic_load_explicit(&s_tcp_connected, memory_order_acquire))
        {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_BENCHMARK_STRESS_SAMPLE_MS));
    }
    return _display_benchmark_should_stop() ? ESP_ERR_INVALID_STATE :
           ESP_ERR_TIMEOUT;
}

static esp_err_t _display_benchmark_stress_save_start_audio(
    display_benchmark_stress_context_t *context)
{
    context->original_audio_state = audio_service_get_state();
    if (context->original_audio_state != AUDIO_SERVICE_STATE_READY &&
            context->original_audio_state != AUDIO_SERVICE_STATE_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = audio_service_get_config(&context->audio_config);
    if (result == ESP_OK)
    {
        result = audio_service_get_volume(&context->original_volume_percent);
    }
    if (result == ESP_OK)
    {
        result = audio_service_get_mute(&context->original_muted);
    }
    if (result == ESP_OK)
    {
        result = audio_service_get_pa(&context->original_pa_enabled);
    }
    if (result != ESP_OK)
    {
        return result;
    }
    context->audio_state_saved = true;
    if (context->original_audio_state == AUDIO_SERVICE_STATE_READY)
    {
        result = audio_service_start();
    }
    if (result == ESP_OK)
    {
        result = audio_service_set_volume(s_config.audio_volume_percent);
    }
    if (result == ESP_OK)
    {
        result = audio_service_set_mute(false);
    }
    if (result == ESP_OK)
    {
        result = audio_service_set_pa(true);
    }
    return result;
}

static esp_err_t _display_benchmark_stress_restore_audio(
    display_benchmark_stress_context_t *context)
{
    if (!context->audio_state_saved)
    {
        return ESP_OK;
    }
    esp_err_t result = ESP_OK;
    audio_service_state_t current_state = audio_service_get_state();
    const bool stop_required = current_state == AUDIO_SERVICE_STATE_ERROR ||
                               (context->original_audio_state ==
                                AUDIO_SERVICE_STATE_READY &&
                                current_state == AUDIO_SERVICE_STATE_RUNNING);
    if (stop_required)
    {
        result = audio_service_stop();
        current_state = audio_service_get_state();
    }
    if (context->original_audio_state == AUDIO_SERVICE_STATE_RUNNING &&
            current_state != AUDIO_SERVICE_STATE_RUNNING)
    {
        const esp_err_t start_result = audio_service_start();
        if (result == ESP_OK)
        {
            result = start_result;
        }
    }
    const esp_err_t volume_result = audio_service_set_volume(
                                        context->original_volume_percent);
    const esp_err_t mute_result = audio_service_set_mute(
                                      context->original_muted);
    const esp_err_t pa_result = audio_service_set_pa(
                                    context->original_pa_enabled);
    if (result == ESP_OK)
    {
        result = volume_result;
    }
    if (result == ESP_OK)
    {
        result = mute_result;
    }
    if (result == ESP_OK)
    {
        result = pa_result;
    }
    if (result == ESP_OK && audio_service_get_state() !=
            context->original_audio_state)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

static void _display_benchmark_stress_preserve_error(
    esp_err_t *result, esp_err_t candidate)
{
    if (*result == ESP_OK)
    {
        *result = candidate;
    }
}

static esp_err_t _display_benchmark_stress_start_audio_workers(
    display_benchmark_stress_context_t *context)
{
    context->audio_tx_worker.context = context;
    esp_err_t result = _display_benchmark_worker_start(
                           &context->audio_tx_worker,
                           _display_benchmark_stress_audio_tx_task,
                           "stress_audio_tx",
                           DISPLAY_BENCHMARK_STRESS_AUDIO_STACK,
                           DISPLAY_BENCHMARK_AUDIO_PRIORITY);
    if (result != ESP_OK)
    {
        return result;
    }
    context->audio_rx_worker.context = context;
    result = _display_benchmark_worker_start(
                 &context->audio_rx_worker,
                 _display_benchmark_stress_audio_rx_task,
                 "stress_audio_rx",
                 DISPLAY_BENCHMARK_STRESS_AUDIO_STACK,
                 DISPLAY_BENCHMARK_AUDIO_PRIORITY);
    return result;
}

static esp_err_t _display_benchmark_stress_start_sampler(
    display_benchmark_stress_context_t *context)
{
    context->sampler_worker.context = context;
    atomic_store_explicit(&context->sampler_enabled, true,
                          memory_order_release);
    const esp_err_t result = _display_benchmark_worker_start(
                                 &context->sampler_worker,
                                 _display_benchmark_stress_sampler_task,
                                 "stress_sampler",
                                 DISPLAY_BENCHMARK_STRESS_SAMPLER_STACK,
                                 DISPLAY_BENCHMARK_SUPERVISOR_PRIORITY);
    if (result != ESP_OK)
    {
        atomic_store_explicit(&context->sampler_enabled, false,
                              memory_order_release);
    }
    return result;
}

static esp_err_t _display_benchmark_stress_close_provisioning(void)
{
    esp_err_t result = provisioning_service_close_window();
    if (result != ESP_OK)
    {
        return result;
    }
    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)DISPLAY_BENCHMARK_STRESS_CLEANUP_MS *
                                1000LL;
    while (esp_timer_get_time() < deadline_us)
    {
        provisioning_service_status_t status = {0};
        result = provisioning_service_get_status(&status);
        if (result == ESP_OK && !status.active &&
                (status.state == PROVISIONING_SERVICE_STATE_IDLE ||
                 status.state == PROVISIONING_SERVICE_STATE_DISABLED))
        {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_BENCHMARK_STRESS_SAMPLE_MS));
    }
    return ESP_ERR_TIMEOUT;
}

static size_t _display_benchmark_stress_median(size_t *values, size_t count)
{
    for (size_t index = 1U; index < count; ++index)
    {
        const size_t value = values[index];
        size_t position = index;
        while (position > 0U && values[position - 1U] > value)
        {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = value;
    }
    if (count == 0U)
    {
        return 0U;
    }
    if ((count & 1U) != 0U)
    {
        return values[count / 2U];
    }
    return values[count / 2U - 1U] / 2U + values[count / 2U] / 2U;
}

static size_t _display_benchmark_stress_maximum_psram_decline(
    const display_benchmark_stress_context_t *context, int64_t duration_us)
{
    size_t maximum_decline = 0U;
    for (int64_t window_start_us = 0;
            window_start_us + DISPLAY_BENCHMARK_STRESS_PSRAM_WINDOW_US <=
            duration_us;
            window_start_us += DISPLAY_BENCHMARK_STRESS_PSRAM_WINDOW_US)
    {
        size_t first_values[64];
        size_t last_values[64];
        size_t first_count = 0U;
        size_t last_count = 0U;
        const int64_t window_end_us = window_start_us +
                                      DISPLAY_BENCHMARK_STRESS_PSRAM_WINDOW_US;
        for (size_t index = 0U; index < context->heap_sample_count; ++index)
        {
            const display_benchmark_stress_heap_sample_t *sample =
                &context->heap_samples[index];
            if (sample->elapsed_us >= window_start_us &&
                    sample->elapsed_us < window_start_us +
                    DISPLAY_BENCHMARK_STRESS_TREND_EDGE_US &&
                    first_count < sizeof(first_values) / sizeof(first_values[0]))
            {
                first_values[first_count++] = sample->psram_free;
            }
            if (sample->elapsed_us >= window_end_us -
                    DISPLAY_BENCHMARK_STRESS_TREND_EDGE_US &&
                    sample->elapsed_us <= window_end_us &&
                    last_count < sizeof(last_values) / sizeof(last_values[0]))
            {
                last_values[last_count++] = sample->psram_free;
            }
        }
        if (first_count == 0U || last_count == 0U)
        {
            return SIZE_MAX;
        }
        const size_t first_median = _display_benchmark_stress_median(
                                        first_values, first_count);
        const size_t last_median = _display_benchmark_stress_median(
                                       last_values, last_count);
        const size_t decline = first_median > last_median ?
                               first_median - last_median : 0U;
        if (decline > maximum_decline)
        {
            maximum_decline = decline;
        }
    }
    return maximum_decline;
}

static bool _display_benchmark_stress_tasks_passed(
    const display_benchmark_stress_context_t *context)
{
    bool passed = true;
    for (size_t index = 0U; index < DISPLAY_BENCHMARK_STRESS_TASK_COUNT;
            ++index)
    {
        const display_benchmark_stress_task_metric_t *metric =
            &context->tasks[index];
        const uint32_t minimum = index == 0U ?
                                 DISPLAY_BENCHMARK_STRESS_RENDER_MINIMUM_HWM :
                                 DISPLAY_BENCHMARK_STRESS_TASK_MINIMUM_HWM;
        const bool psram_required = index == 0U || index >= 5U;
        const bool task_passed = metric->sample_count > 0U &&
                                 metric->found_in_every_sample &&
                                 metric->minimum_high_water >= minimum &&
                                 (!psram_required ||
                                  metric->stack_in_psram_in_every_sample);
        LOG_I("c_ext_stress task=%s result=%s samples=%u found=%u stack_psram=%u min_hwm=%u required_hwm=%u required_psram=%u",
              metric->name, task_passed ? "PASS" : "FAIL",
              (unsigned)metric->sample_count,
              metric->found_in_every_sample ? 1U : 0U,
              metric->stack_in_psram_in_every_sample ? 1U : 0U,
              (unsigned)metric->minimum_high_water, (unsigned)minimum,
              psram_required ? 1U : 0U);
        passed = passed && task_passed;
    }
    return passed;
}

static bool _display_benchmark_stress_effects_passed(
    const app_manager_display_benchmark_report_t *report)
{
    for (size_t index = 0U;
            index < DISPLAY_BENCHMARK_STRESS_EFFECT_COUNT; ++index)
    {
        const size_t report_index =
            (size_t)(s_stress_effects[index] - APP_MANAGER_TRANSITION_NONE);
        const app_manager_display_effect_benchmark_report_t *effect =
            &report->effects[report_index];
        if (effect->transition_start_count == 0U ||
                effect->transition_complete_count !=
                effect->transition_start_count ||
                effect->transition_cancel_count != 0U)
        {
            return false;
        }
    }
    return true;
}
#endif

static esp_err_t _display_benchmark_start_load(display_benchmark_load_t load,
        bool *audio_started, int64_t *tcp_started_us)
{
    esp_err_t result = ESP_OK;
    if (_display_benchmark_load_requires_audio(load))
    {
        result = audio_service_start();
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_audio_error, result);
            return result;
        }
        *audio_started = true;

        result = _display_benchmark_worker_start(&s_audio_worker,
                 _display_benchmark_audio_task, "display_audio",
                 DISPLAY_BENCHMARK_AUDIO_STACK,
                 DISPLAY_BENCHMARK_AUDIO_PRIORITY);
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_control_error, result);
            return result;
        }
    }

    if (_display_benchmark_load_requires_tcp(load))
    {
        const int64_t started_us = esp_timer_get_time();
        result = _display_benchmark_worker_start(&s_tcp_worker,
                 _display_benchmark_tcp_task, "display_tcp",
                 DISPLAY_BENCHMARK_TCP_STACK,
                 DISPLAY_BENCHMARK_TCP_PRIORITY);
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_control_error, result);
        }
        else
        {
            *tcp_started_us = started_us;
        }
    }
    return result;
}

static void _display_benchmark_stop_load(bool audio_started)
{
    atomic_store_explicit(&s_load_stop_requested, true, memory_order_release);
    _display_benchmark_worker_stop(&s_tcp_worker);
    _display_benchmark_worker_stop(&s_audio_worker);
    if (audio_started)
    {
        const esp_err_t result = audio_service_stop();
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_audio_error, result);
        }
    }
}

static esp_err_t _display_benchmark_begin_profile(void)
{
    return app_manager_display_diagnostics_begin_benchmark(
               DISPLAY_BENCHMARK_FPS_CROSS_CHECK,
               DISPLAY_BENCHMARK_MINIMUM_DMA_LARGEST);
}

static esp_err_t _display_benchmark_run_characterization_profile(
    display_benchmark_page_t *page,
    app_manager_display_benchmark_report_t *report, bool *completed)
{
    esp_err_t result = _display_benchmark_begin_profile();
    if (result != ESP_OK)
    {
        return result;
    }

    size_t completed_effects = 0U;
    for (size_t index = 0U;
            index < DISPLAY_BENCHMARK_CHARACTERIZATION_EFFECT_COUNT; ++index)
    {
        const int64_t deadline_us = esp_timer_get_time() +
                                    _display_benchmark_effect_duration_us();
        while (!atomic_load_explicit(&s_stop_requested,
                                     memory_order_acquire) &&
                atomic_load_explicit(&s_workload_error,
                                     memory_order_relaxed) == ESP_OK &&
                esp_timer_get_time() < deadline_us)
        {
            _display_benchmark_check_wifi();
            result = _display_benchmark_navigate_effect(
                         page, s_characterization_effects[index]);
            if (result != ESP_OK)
            {
                break;
            }
        }
        if (result != ESP_OK ||
                atomic_load_explicit(&s_stop_requested,
                                     memory_order_acquire) ||
                atomic_load_explicit(&s_workload_error,
                                     memory_order_relaxed) != ESP_OK)
        {
            break;
        }
        completed_effects++;
    }

    const esp_err_t end_result =
        app_manager_display_diagnostics_end_benchmark(report);
    *completed = completed_effects ==
                 DISPLAY_BENCHMARK_CHARACTERIZATION_EFFECT_COUNT;
    return result != ESP_OK ? result : end_result;
}

static esp_err_t _display_benchmark_run_stress_profile(
    display_benchmark_page_t *page,
    app_manager_display_benchmark_report_t *report, bool *completed)
{
    esp_err_t result = _display_benchmark_begin_profile();
    if (result != ESP_OK)
    {
        return result;
    }

    const int64_t started_us = esp_timer_get_time();
    while (!atomic_load_explicit(&s_stop_requested, memory_order_acquire) &&
            atomic_load_explicit(&s_workload_error,
                                 memory_order_relaxed) == ESP_OK)
    {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        if (elapsed_us >= _display_benchmark_stress_duration_us())
        {
            break;
        }
        _display_benchmark_check_wifi();
        result = _display_benchmark_navigate_stress(page, elapsed_us);
        if (result != ESP_OK)
        {
            break;
        }
    }

    const esp_err_t end_result =
        app_manager_display_diagnostics_end_benchmark(report);
    *completed = !atomic_load_explicit(&s_stop_requested,
                                       memory_order_relaxed) &&
                 esp_timer_get_time() - started_us >=
                 _display_benchmark_stress_duration_us();
    return result != ESP_OK ? result : end_result;
}

static esp_err_t _display_benchmark_run_profile(
    display_benchmark_page_t *page,
    app_manager_display_benchmark_report_t *report, bool *completed)
{
    return s_config.mode == DISPLAY_BENCHMARK_MODE_CHARACTERIZATION ?
           _display_benchmark_run_characterization_profile(
               page, report, completed) :
           _display_benchmark_run_stress_profile(page, report, completed);
}

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
static void _display_benchmark_c_ext_stress_supervisor(void)
{
    display_benchmark_stability_summary_t summary =
    {
        .diagnostics_passed = true,
    };
    display_benchmark_performance_t performance =
        DISPLAY_BENCHMARK_PERFORMANCE_FAIL;
    display_benchmark_stress_context_t *context = NULL;
    app_manager_display_benchmark_report_t *report = NULL;
    provisioning_service_diagnostics_t provisioning_start = {0};
    provisioning_service_diagnostics_t provisioning_end = {0};
    bool diagnostics_started = false;
    bool report_available = false;
    bool completed = false;
    bool benchmark_started = false;
    int64_t measurement_started_us = 0;
    int64_t measurement_stopped_us = 0;
    esp_err_t result = ESP_OK;
    esp_err_t cleanup_result = ESP_OK;

    context = heap_caps_malloc(sizeof(*context), DISPLAY_BENCHMARK_TASK_CAPS);
    if (context == NULL)
    {
        _display_benchmark_record_error(&s_control_error, ESP_ERR_NO_MEM);
        return;
    }
    memset(context, 0, sizeof(*context));
    s_stress_context = context;
    for (size_t index = 0U; index < DISPLAY_BENCHMARK_STRESS_TASK_COUNT;
            ++index)
    {
        context->tasks[index].name = s_stress_task_names[index];
    }
    const int64_t configured_duration_us =
        _display_benchmark_stress_duration_us();
    context->heap_sample_capacity =
        (size_t)(configured_duration_us /
                 ((int64_t)DISPLAY_BENCHMARK_STRESS_SAMPLE_MS * 1000LL)) + 8U;
    context->heap_samples = heap_caps_malloc(
                                context->heap_sample_capacity *
                                sizeof(*context->heap_samples),
                                DISPLAY_BENCHMARK_TASK_CAPS);
    report = heap_caps_malloc(sizeof(*report), DISPLAY_BENCHMARK_TASK_CAPS);
    if (context->heap_samples == NULL || report == NULL)
    {
        _display_benchmark_record_error(&s_control_error, ESP_ERR_NO_MEM);
        goto cleanup;
    }

    _display_benchmark_stress_set_phase(
        context, DISPLAY_BENCHMARK_STRESS_PHASE_BEGIN);
    while (!_display_benchmark_should_stop() &&
            !_display_benchmark_wifi_ready())
    {
        (void)ulTaskNotifyTake(
            pdTRUE, pdMS_TO_TICKS(DISPLAY_BENCHMARK_WIFI_POLL_MS));
    }
    if (_display_benchmark_should_stop())
    {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    atomic_store_explicit(&s_wifi_was_ready, true, memory_order_relaxed);
    atomic_store_explicit(&s_wifi_monitoring_active, true,
                          memory_order_release);
    benchmark_started = true;
    _display_benchmark_log_config();

    _display_benchmark_stress_set_phase(
        context, DISPLAY_BENCHMARK_STRESS_PHASE_LOAD_START);
    result = _display_benchmark_worker_start(
                 &s_tcp_worker, _display_benchmark_tcp_task, "display_tcp",
                 DISPLAY_BENCHMARK_TCP_STACK,
                 DISPLAY_BENCHMARK_TCP_PRIORITY);
    if (result == ESP_OK)
    {
        result = _display_benchmark_stress_wait_for_tcp();
    }
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }
    result = _display_benchmark_stress_save_start_audio(context);
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_audio_error, result);
        goto cleanup;
    }
    result = _display_benchmark_stress_start_audio_workers(context);
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }

    _display_benchmark_stress_set_phase(
        context, DISPLAY_BENCHMARK_STRESS_PHASE_PROVISIONING_WAIT);
    result = _display_benchmark_stress_open_provisioning();
    if (result == ESP_OK)
    {
        result = _display_benchmark_stress_wait_for_provisioning(context);
    }
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }

    _display_benchmark_stress_set_phase(
        context, DISPLAY_BENCHMARK_STRESS_PHASE_WARMUP);
    result = _display_benchmark_stress_start_sampler(context);
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }
    display_benchmark_page_t page = DISPLAY_BENCHMARK_PAGE_HOME;
    result = _display_benchmark_stress_warmup(&page);
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }
    context->warm_internal_free = heap_caps_get_free_size(
                                      DISPLAY_BENCHMARK_STRESS_INTERNAL_CAPS);
    context->warm_psram_free = heap_caps_get_free_size(
                                   DISPLAY_BENCHMARK_STRESS_PSRAM_CAPS);
    result = provisioning_service_get_diagnostics(&provisioning_start);
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }

    atomic_store_explicit(&s_tcp_transmit_bytes, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_receive_bytes, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_interruption_us, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_maximum_pacing_lag_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_tcp_reconnect_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_pacing_late_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_wifi_disconnect_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_tcp_error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&s_audio_error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&context->audio_transmit_bytes, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_receive_bytes, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_transmit_short_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_receive_short_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_transmit_timeout_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_receive_timeout_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_transmit_error_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_receive_error_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_transmit_deadline_miss_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->audio_receive_deadline_miss_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&context->microphone_nonzero, false,
                          memory_order_relaxed);

    result = _display_benchmark_begin_profile();
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto cleanup;
    }
    diagnostics_started = true;
    measurement_started_us = esp_timer_get_time();
    atomic_store_explicit(&context->measure_start_us, measurement_started_us,
                          memory_order_release);
    _display_benchmark_stress_set_phase(
        context, DISPLAY_BENCHMARK_STRESS_PHASE_MEASURE);
    result = _display_benchmark_stress_run_routes(
                 context, measurement_started_us, &completed);
    measurement_stopped_us = esp_timer_get_time();
    atomic_store_explicit(&context->sampler_enabled, false,
                          memory_order_release);
    _display_benchmark_worker_stop(&context->sampler_worker);
    _display_benchmark_stress_observe_ble(context, measurement_stopped_us);
    atomic_store_explicit(&context->measure_start_us, 0,
                          memory_order_release);
    const esp_err_t end_result =
        app_manager_display_diagnostics_end_benchmark(report);
    diagnostics_started = false;
    if (result == ESP_OK)
    {
        result = end_result;
    }
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
    }
    else
    {
        report_available = true;
        _display_benchmark_log_profile(DISPLAY_BENCHMARK_LOAD_FULL, report);
        _display_benchmark_accumulate_stability(&summary, report);
        performance = _display_benchmark_grade_profile(report);
    }
    const esp_err_t provisioning_result =
        provisioning_service_get_diagnostics(&provisioning_end);
    if (provisioning_result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error,
                                        provisioning_result);
    }

cleanup:
    if (diagnostics_started)
    {
        const esp_err_t end_result =
            app_manager_display_diagnostics_end_benchmark(report);
        if (cleanup_result == ESP_OK)
        {
            cleanup_result = end_result;
        }
    }
    atomic_store_explicit(&s_wifi_monitoring_active, false,
                          memory_order_release);
    if (context != NULL)
    {
        _display_benchmark_stress_set_phase(
            context, DISPLAY_BENCHMARK_STRESS_PHASE_CLEANUP);
    }
    atomic_store_explicit(&s_load_stop_requested, true, memory_order_release);
    if (context != NULL)
    {
        atomic_store_explicit(&context->sampler_enabled, false,
                              memory_order_release);
        _display_benchmark_worker_stop(&context->sampler_worker);
        _display_benchmark_worker_stop(&context->audio_rx_worker);
        _display_benchmark_worker_stop(&context->audio_tx_worker);
    }
    _display_benchmark_worker_stop(&s_tcp_worker);
    if (provisioning_service_is_active())
    {
        const esp_err_t provisioning_close_result =
            _display_benchmark_stress_close_provisioning();
        _display_benchmark_stress_preserve_error(
            &cleanup_result, provisioning_close_result);
    }
    if (!atomic_load_explicit(&s_stop_requested, memory_order_acquire) &&
            benchmark_started)
    {
        const esp_err_t home_result = _display_benchmark_stress_navigate(
                                          APP_MANAGER_ID_HOME, "root",
                                          APP_MANAGER_TRANSITION_FADE);
        _display_benchmark_stress_preserve_error(&cleanup_result, home_result);
        const esp_err_t apps_result =
            _display_benchmark_stress_cleanup_background();
        _display_benchmark_stress_preserve_error(&cleanup_result, apps_result);
    }
    if (context != NULL)
    {
        const esp_err_t audio_result =
            _display_benchmark_stress_restore_audio(context);
        _display_benchmark_stress_preserve_error(&cleanup_result, audio_result);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_BENCHMARK_STRESS_SAMPLE_MS));
        context->cleanup_internal_free = heap_caps_get_free_size(
                                             DISPLAY_BENCHMARK_STRESS_INTERNAL_CAPS);
        context->cleanup_psram_free = heap_caps_get_free_size(
                                          DISPLAY_BENCHMARK_STRESS_PSRAM_CAPS);
        _display_benchmark_stress_set_phase(
            context, DISPLAY_BENCHMARK_STRESS_PHASE_END);
    }
    if (cleanup_result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, cleanup_result);
    }

    if (benchmark_started && context != NULL)
    {
        const uint64_t measurement_duration_us =
            measurement_started_us > 0 &&
            measurement_stopped_us > measurement_started_us ?
            (uint64_t)(measurement_stopped_us - measurement_started_us) : 0U;
        display_benchmark_tcp_report_t tcp_report =
        {
            .transmit_bytes = atomic_load_explicit(
                &s_tcp_transmit_bytes,
                memory_order_relaxed),
            .receive_bytes = atomic_load_explicit(
                &s_tcp_receive_bytes,
                memory_order_relaxed),
            .target_bytes = (uint64_t)s_config.rate_kbit_s *
            measurement_duration_us / 8000U,
            .active_duration_us = measurement_duration_us,
            .interruption_us = atomic_load_explicit(
                &s_tcp_interruption_us,
                memory_order_relaxed),
            .maximum_pacing_lag_us = atomic_load_explicit(
                &s_tcp_maximum_pacing_lag_us,
                memory_order_relaxed),
            .reconnect_count = atomic_load_explicit(
                &s_tcp_reconnect_count,
                memory_order_relaxed),
            .pacing_late_count = atomic_load_explicit(
                &s_tcp_pacing_late_count,
                memory_order_relaxed),
            .required = true,
        };
        const uint64_t minimum_tcp_bytes =
            (tcp_report.target_bytes *
             DISPLAY_BENCHMARK_STRESS_MINIMUM_PERCENT + 99U) / 100U;
        tcp_report.throughput_passed = measurement_duration_us > 0U &&
                                       tcp_report.transmit_bytes >=
                                       minimum_tcp_bytes &&
                                       tcp_report.receive_bytes >=
                                       minimum_tcp_bytes;

        const uint64_t bytes_per_second =
            (uint64_t)context->audio_config.sample_rate_hz *
            context->audio_config.channels *
            (context->audio_config.bits_per_sample / 8U);
        const uint64_t audio_target_bytes =
            bytes_per_second * measurement_duration_us / 1000000U;
        const uint64_t minimum_audio_bytes =
            (audio_target_bytes * DISPLAY_BENCHMARK_STRESS_MINIMUM_PERCENT +
             99U) / 100U;
        const uint64_t audio_transmit_bytes = atomic_load_explicit(
                &context->audio_transmit_bytes, memory_order_relaxed);
        const uint64_t audio_receive_bytes = atomic_load_explicit(
                &context->audio_receive_bytes, memory_order_relaxed);
        const uint32_t audio_transmit_short_count = atomic_load_explicit(
                &context->audio_transmit_short_count, memory_order_relaxed);
        const uint32_t audio_receive_short_count = atomic_load_explicit(
                &context->audio_receive_short_count, memory_order_relaxed);
        const uint32_t audio_transmit_timeout_count = atomic_load_explicit(
                &context->audio_transmit_timeout_count, memory_order_relaxed);
        const uint32_t audio_receive_timeout_count = atomic_load_explicit(
                &context->audio_receive_timeout_count, memory_order_relaxed);
        const uint32_t audio_transmit_error_count = atomic_load_explicit(
                &context->audio_transmit_error_count, memory_order_relaxed);
        const uint32_t audio_receive_error_count = atomic_load_explicit(
                &context->audio_receive_error_count, memory_order_relaxed);
        const uint32_t audio_transmit_deadline_miss_count =
            atomic_load_explicit(
                &context->audio_transmit_deadline_miss_count,
                memory_order_relaxed);
        const uint32_t audio_receive_deadline_miss_count =
            atomic_load_explicit(
                &context->audio_receive_deadline_miss_count,
                memory_order_relaxed);
        const uint32_t audio_fault_count =
            audio_transmit_short_count + audio_receive_short_count +
            audio_transmit_timeout_count + audio_receive_timeout_count +
            audio_transmit_error_count + audio_receive_error_count +
            audio_transmit_deadline_miss_count +
            audio_receive_deadline_miss_count;
        const bool audio_passed = audio_target_bytes > 0U &&
                                  audio_transmit_bytes >= minimum_audio_bytes &&
                                  audio_receive_bytes >= minimum_audio_bytes &&
                                  audio_fault_count == 0U &&
                                  atomic_load_explicit(
                                      &context->microphone_nonzero,
                                      memory_order_relaxed);

        const uint64_t protected_success_count =
            provisioning_end.protected_success_count >=
            provisioning_start.protected_success_count ?
            provisioning_end.protected_success_count -
            provisioning_start.protected_success_count : 0U;
        const uint64_t protected_failure_count =
            provisioning_end.protected_failure_count >=
            provisioning_start.protected_failure_count ?
            provisioning_end.protected_failure_count -
            provisioning_start.protected_failure_count : UINT64_MAX;
        const uint64_t snapshot_success_count =
            provisioning_end.snapshot_success_count >=
            provisioning_start.snapshot_success_count ?
            provisioning_end.snapshot_success_count -
            provisioning_start.snapshot_success_count : 0U;
        const uint64_t theoretical_snapshots = measurement_duration_us /
                                               2000000U;
        const uint64_t minimum_snapshots =
            (theoretical_snapshots *
             DISPLAY_BENCHMARK_STRESS_MINIMUM_PERCENT + 99U) / 100U;
        const bool ble_passed = context->ble_disconnect_count == 0U &&
                                context->ble_reconnect_count == 0U &&
                                protected_failure_count == 0U &&
                                protected_success_count >= minimum_snapshots &&
                                snapshot_success_count >= minimum_snapshots &&
                                provisioning_end.last_snapshot_request_id !=
                                0U &&
                                context->maximum_ble_success_interval_us <=
                                DISPLAY_BENCHMARK_STRESS_BLE_INTERVAL_US &&
                                context->maximum_ble_success_idle_us <=
                                DISPLAY_BENCHMARK_STRESS_BLE_INTERVAL_US &&
                                provisioning_end.worker_found &&
                                provisioning_end.worker_stack_high_water >=
                                DISPLAY_BENCHMARK_STRESS_TASK_MINIMUM_HWM;

        const bool heap_passed = context->heap_sampled &&
                                 context->minimum_internal_free > 0U &&
                                 context->minimum_internal_largest > 0U &&
                                 context->minimum_dma_free > 0U &&
                                 context->minimum_dma_largest >=
                                 DISPLAY_BENCHMARK_MINIMUM_DMA_LARGEST &&
                                 context->minimum_psram_free > 0U &&
                                 context->minimum_psram_largest > 0U;
        const size_t maximum_psram_decline =
            _display_benchmark_stress_maximum_psram_decline(
                context, (int64_t)measurement_duration_us);
        const bool trend_passed = maximum_psram_decline != SIZE_MAX &&
                                  maximum_psram_decline <=
                                  DISPLAY_BENCHMARK_STRESS_PSRAM_TREND_BYTES;
        const bool recovery_passed =
            context->cleanup_internal_free +
            DISPLAY_BENCHMARK_STRESS_INTERNAL_RECOVERY_BYTES >=
            context->warm_internal_free &&
            context->cleanup_psram_free +
            DISPLAY_BENCHMARK_STRESS_PSRAM_RECOVERY_BYTES >=
            context->warm_psram_free;
        const bool tasks_passed =
            _display_benchmark_stress_tasks_passed(context);
        const uint32_t expected_route_mask =
            (1U << DISPLAY_BENCHMARK_STRESS_ROUTE_COUNT) - 1U;
        const uint32_t expected_effect_mask =
            (1U << DISPLAY_BENCHMARK_STRESS_EFFECT_COUNT) - 1U;
        const bool navigation_passed = report_available &&
                                       context->route_visit_mask ==
                                       expected_route_mask &&
                                       context->effect_visit_mask ==
                                       expected_effect_mask &&
                                       _display_benchmark_stress_effects_passed(
                                           report);
        const bool stress_passed = completed && cleanup_result == ESP_OK &&
                                   report_available && report->passed &&
                                   performance >=
                                   DISPLAY_BENCHMARK_PERFORMANCE_FLOOR &&
                                   tcp_report.throughput_passed &&
                                   tcp_report.reconnect_count == 0U &&
                                   atomic_load_explicit(
                                       &s_wifi_disconnect_count,
                                       memory_order_relaxed) == 0U &&
                                   audio_passed && ble_passed && heap_passed &&
                                   trend_passed && recovery_passed &&
                                   tasks_passed && navigation_passed;
        summary.diagnostics_passed = summary.diagnostics_passed &&
                                     stress_passed;
        LOG_I("c_ext_stress ble result=%s protected_success=%llu protected_failure=%llu snapshot_success=%llu theoretical=%llu disconnects=%u reconnects=%u max_success_interval_us=%llu max_success_idle_us=%llu last_request_id=%llu worker_found=%u worker_hwm=%u",
              ble_passed ? "PASS" : "FAIL",
              (unsigned long long)protected_success_count,
              (unsigned long long)protected_failure_count,
              (unsigned long long)snapshot_success_count,
              (unsigned long long)theoretical_snapshots,
              (unsigned)context->ble_disconnect_count,
              (unsigned)context->ble_reconnect_count,
              (unsigned long long)context->maximum_ble_success_interval_us,
              (unsigned long long)context->maximum_ble_success_idle_us,
              (unsigned long long)provisioning_end.last_snapshot_request_id,
              provisioning_end.worker_found ? 1U : 0U,
              (unsigned)provisioning_end.worker_stack_high_water);
        LOG_I("c_ext_stress audio result=%s tx_bytes=%llu rx_bytes=%llu target_bytes=%llu tx_short=%u rx_short=%u tx_timeout=%u rx_timeout=%u tx_error=%u rx_error=%u tx_deadline_miss=%u rx_deadline_miss=%u faults=%u mic_nonzero=%u",
              audio_passed ? "PASS" : "FAIL",
              (unsigned long long)audio_transmit_bytes,
              (unsigned long long)audio_receive_bytes,
              (unsigned long long)audio_target_bytes,
              (unsigned)audio_transmit_short_count,
              (unsigned)audio_receive_short_count,
              (unsigned)audio_transmit_timeout_count,
              (unsigned)audio_receive_timeout_count,
              (unsigned)audio_transmit_error_count,
              (unsigned)audio_receive_error_count,
              (unsigned)audio_transmit_deadline_miss_count,
              (unsigned)audio_receive_deadline_miss_count,
              (unsigned)audio_fault_count,
              atomic_load_explicit(&context->microphone_nonzero,
                                   memory_order_relaxed) ? 1U : 0U);
        LOG_I("c_ext_stress heap result=%s min_internal_free=%u min_internal_largest=%u min_dma_free=%u min_dma_largest=%u min_psram_free=%u min_psram_largest=%u psram_max_decline=%u trend_ok=%u warm_internal=%u cleanup_internal=%u warm_psram=%u cleanup_psram=%u recovery_ok=%u samples=%u",
              heap_passed && trend_passed && recovery_passed ? "PASS" : "FAIL",
              (unsigned)context->minimum_internal_free,
              (unsigned)context->minimum_internal_largest,
              (unsigned)context->minimum_dma_free,
              (unsigned)context->minimum_dma_largest,
              (unsigned)context->minimum_psram_free,
              (unsigned)context->minimum_psram_largest,
              maximum_psram_decline == SIZE_MAX ? UINT_MAX :
              (unsigned)maximum_psram_decline,
              trend_passed ? 1U : 0U,
              (unsigned)context->warm_internal_free,
              (unsigned)context->cleanup_internal_free,
              (unsigned)context->warm_psram_free,
              (unsigned)context->cleanup_psram_free,
              recovery_passed ? 1U : 0U,
              (unsigned)context->heap_sample_count);
        LOG_I("c_ext_stress summary result=%s completed=%u cleanup=0x%x routes=0x%x effects=0x%x navigation_ok=%u tasks_ok=%u performance=%s duration_us=%llu",
              stress_passed ? "PASS" : "FAIL", completed ? 1U : 0U,
              (unsigned)cleanup_result, (unsigned)context->route_visit_mask,
              (unsigned)context->effect_visit_mask,
              navigation_passed ? 1U : 0U, tasks_passed ? 1U : 0U,
              _display_benchmark_performance_name(performance),
              (unsigned long long)measurement_duration_us);
        _display_benchmark_log_final(
            &summary, &tcp_report, DISPLAY_BENCHMARK_LOAD_FULL, performance,
            completed,
            atomic_load_explicit(&s_stop_requested, memory_order_relaxed));
    }
    heap_caps_free(report);
    heap_caps_free(context != NULL ? context->heap_samples : NULL);
    heap_caps_free(context);
    s_stress_context = NULL;
}
#endif

static void _display_benchmark_supervisor_task(void *arg)
{
    (void)arg;
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    if (_display_benchmark_is_c_ext_stress())
    {
        _display_benchmark_c_ext_stress_supervisor();
        (void)xSemaphoreGive(s_supervisor_stopped);
        vTaskSuspend(NULL);
        return;
    }
#endif
    const display_benchmark_load_t load = s_config.load;
    const bool tcp_required = _display_benchmark_load_requires_tcp(load);
    bool audio_started = false;
    bool benchmark_started = false;
    bool completed = false;
    bool report_available = false;
    int64_t tcp_started_us = 0;
    display_benchmark_performance_t performance =
        DISPLAY_BENCHMARK_PERFORMANCE_TARGET;
    display_benchmark_stability_summary_t summary =
    {
        .diagnostics_passed = true,
    };
    app_manager_display_benchmark_report_t *report = NULL;

    while (tcp_required &&
            !atomic_load_explicit(&s_stop_requested, memory_order_acquire) &&
            !_display_benchmark_wifi_ready())
    {
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(DISPLAY_BENCHMARK_WIFI_POLL_MS));
    }
    if (atomic_load_explicit(&s_stop_requested, memory_order_acquire))
    {
        goto exit;
    }
    if (tcp_required)
    {
        atomic_store_explicit(&s_wifi_was_ready, true, memory_order_relaxed);
        atomic_store_explicit(&s_wifi_monitoring_active, true,
                              memory_order_release);
        _display_benchmark_check_wifi();
    }
    benchmark_started = true;
    _display_benchmark_log_config();

    report = heap_caps_malloc(sizeof(*report), DISPLAY_BENCHMARK_TASK_CAPS);
    if (report == NULL)
    {
        _display_benchmark_record_error(&s_control_error, ESP_ERR_NO_MEM);
        goto exit;
    }

    display_benchmark_page_t page = DISPLAY_BENCHMARK_PAGE_HOME;
    esp_err_t result = ESP_OK;
    if (s_config.mode == DISPLAY_BENCHMARK_MODE_CHARACTERIZATION)
    {
        bool display_only_completed = false;
        memset(report, 0, sizeof(*report));
        result = _display_benchmark_run_profile(
                     &page, report, &display_only_completed);
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_control_error, result);
            goto exit;
        }
        report_available = true;
        _display_benchmark_log_profile(DISPLAY_BENCHMARK_LOAD_DISPLAY_ONLY,
                                       report);
        _display_benchmark_accumulate_stability(&summary, report);
        performance = _display_benchmark_worst_performance(
                          performance,
                          _display_benchmark_grade_profile(report));
        if (!display_only_completed)
        {
            goto exit;
        }

        result = _display_benchmark_start_load(load, &audio_started,
                                               &tcp_started_us);
        if (result != ESP_OK)
        {
            goto exit;
        }
        bool load_completed = false;
        memset(report, 0, sizeof(*report));
        result = _display_benchmark_run_profile(&page, report,
                                                &load_completed);
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_control_error, result);
            goto exit;
        }
        _display_benchmark_log_profile(load, report);
        _display_benchmark_accumulate_stability(&summary, report);
        performance = _display_benchmark_worst_performance(
                          performance,
                          _display_benchmark_grade_profile(report));
        completed = display_only_completed && load_completed;
    }
    else
    {
        result = _display_benchmark_start_load(load, &audio_started,
                                               &tcp_started_us);
        if (result != ESP_OK)
        {
            goto exit;
        }
        memset(report, 0, sizeof(*report));
        result = _display_benchmark_run_profile(&page, report, &completed);
        if (result != ESP_OK)
        {
            _display_benchmark_record_error(&s_control_error, result);
            goto exit;
        }
        report_available = true;
        _display_benchmark_log_profile(load, report);
        _display_benchmark_accumulate_stability(&summary, report);
        performance = _display_benchmark_grade_profile(report);
    }

exit:
    atomic_store_explicit(&s_wifi_monitoring_active, false,
                          memory_order_release);
    const int64_t tcp_stopped_us = esp_timer_get_time();
    _display_benchmark_stop_load(audio_started);
    const uint64_t tcp_active_duration_us =
        tcp_started_us > 0 && tcp_stopped_us > tcp_started_us ?
        (uint64_t)(tcp_stopped_us - tcp_started_us) : 0U;
    const display_benchmark_tcp_report_t tcp_report =
        _display_benchmark_tcp_finish_report(tcp_active_duration_us,
            tcp_required);
    if (benchmark_started)
    {
        if (!report_available)
        {
            summary.diagnostics_passed = false;
        }
        if (!completed)
        {
            performance = DISPLAY_BENCHMARK_PERFORMANCE_FAIL;
        }
        _display_benchmark_log_final(
            &summary, &tcp_report, load, performance, completed,
            atomic_load_explicit(&s_stop_requested, memory_order_relaxed));
    }
    heap_caps_free(report);
    (void)xSemaphoreGive(s_supervisor_stopped);
    vTaskSuspend(NULL);
}

static esp_err_t _display_benchmark_unsubscribe_wifi(void)
{
    if (s_wifi_subscription == EVENT_BUS_SUB_HANDLE_INVALID)
    {
        return ESP_OK;
    }
    const esp_err_t result = event_bus_unsubscribe(s_wifi_subscription);
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
    {
        s_wifi_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
        return ESP_OK;
    }
    return result;
}

esp_err_t display_benchmark_start(const display_benchmark_config_t *config)
{
    if (!_display_benchmark_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_supervisor_task != NULL || s_supervisor_stopped != NULL ||
            s_wifi_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_ipv4_host, config->ipv4_host,
           strlen(config->ipv4_host) + 1U);
    s_config = *config;
    s_config.ipv4_host = s_ipv4_host;
    s_supervisor_stopped = xSemaphoreCreateBinary();
    if (s_supervisor_stopped == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_load_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_workload_error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&s_control_error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&s_audio_error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_transmit_bytes, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_receive_bytes, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_interruption_us, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_maximum_pacing_lag_us, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_tcp_disconnected_since_us, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&s_tcp_reconnect_count, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_tcp_pacing_late_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_wifi_monitoring_active, false,
                          memory_order_relaxed);
    atomic_store_explicit(&s_wifi_was_ready, false, memory_order_relaxed);
    atomic_store_explicit(&s_wifi_disconnect_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_tcp_connected, false, memory_order_relaxed);
    esp_err_t result = ESP_OK;
    if (_display_benchmark_load_requires_tcp(
                s_config.load))
    {
        result = event_bus_subscribe(
                     CONNECTIVITY_MANAGER_MSG,
                     CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                     _display_benchmark_wifi_event, NULL,
                     EVENT_BUS_DISPATCH_PUBLISHER,
                     &s_wifi_subscription);
        if (result != ESP_OK)
        {
            vSemaphoreDelete(s_supervisor_stopped);
            s_supervisor_stopped = NULL;
            return result;
        }
    }
    if (xTaskCreateWithCaps(_display_benchmark_supervisor_task,
                            "display_bench",
                            DISPLAY_BENCHMARK_SUPERVISOR_STACK, NULL,
                            DISPLAY_BENCHMARK_SUPERVISOR_PRIORITY,
                            &s_supervisor_task,
                            DISPLAY_BENCHMARK_TASK_CAPS) == pdPASS)
    {
        return ESP_OK;
    }
    s_supervisor_task = NULL;
    vSemaphoreDelete(s_supervisor_stopped);
    s_supervisor_stopped = NULL;
    (void)_display_benchmark_unsubscribe_wifi();
    return ESP_ERR_NO_MEM;
}

esp_err_t display_benchmark_stop(void)
{
    if (s_supervisor_task != NULL)
    {
        atomic_store_explicit(&s_stop_requested, true, memory_order_release);
        if (xTaskNotifyGive(s_supervisor_task) != pdPASS ||
                xSemaphoreTake(s_supervisor_stopped, portMAX_DELAY) != pdPASS)
        {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDeleteWithCaps(s_supervisor_task);
        s_supervisor_task = NULL;
        vSemaphoreDelete(s_supervisor_stopped);
        s_supervisor_stopped = NULL;
    }
    return _display_benchmark_unsubscribe_wifi();
}

#else

esp_err_t display_benchmark_start(const display_benchmark_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t display_benchmark_stop(void)
{
    return ESP_OK;
}

#endif

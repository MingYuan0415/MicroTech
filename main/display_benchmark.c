#define DBG_TAG "display_bench"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "display_benchmark.h"

#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK

#include "app_manager.h"
#include "app_manager_display_diagnostics.h"
#include "audio_service.h"
#include "wifi_service.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define DISPLAY_BENCHMARK_SUPERVISOR_STACK       4096U
#define DISPLAY_BENCHMARK_AUDIO_STACK            4096U
#define DISPLAY_BENCHMARK_TCP_STACK              4096U
#define DISPLAY_BENCHMARK_SUPERVISOR_PRIORITY    1U
#define DISPLAY_BENCHMARK_AUDIO_PRIORITY         1U
#define DISPLAY_BENCHMARK_TCP_PRIORITY           2U
#define DISPLAY_BENCHMARK_SPI_QUEUE_DEPTH        2U
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
#ifndef DISPLAY_BENCHMARK_DURATION_US
#define DISPLAY_BENCHMARK_DURATION_US \
        ((int64_t)CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_DURATION_SEC * 1000000LL)
#endif
#ifndef DISPLAY_BENCHMARK_PHASE_US
    #define DISPLAY_BENCHMARK_PHASE_US            10000000LL
#endif
#ifndef DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US
#define DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US \
        ((int64_t)CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_DURATION_SEC * \
         1000000LL)
#endif

#define DISPLAY_BENCHMARK_CHARACTERIZATION_EFFECT_COUNT 5U
#define DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_COUNT   2U

#ifndef CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_FULL
    #define CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_FULL 0
#endif
#ifndef CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_AUDIO_ONLY
    #define CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_AUDIO_ONLY 0
#endif
#ifndef CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_TCP_ONLY
    #define CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_TCP_ONLY 0
#endif
#ifndef CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG
    #define CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG 0
#endif

#if CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED
    #define DISPLAY_BENCHMARK_COLOR_FORMAT "RGB565_SWAPPED"
#else
    #define DISPLAY_BENCHMARK_COLOR_FORMAT "RGB565"
#endif

#if defined(CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT) && \
    CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT
    #define DISPLAY_BENCHMARK_DIRECT_DMA_ENABLED 1U
#else
    #define DISPLAY_BENCHMARK_DIRECT_DMA_ENABLED 0U
#endif

#if defined(CONFIG_BSP_DISPLAY_TE_SYNC) && CONFIG_BSP_DISPLAY_TE_SYNC
    #define DISPLAY_BENCHMARK_TE_ENABLED 1U
#else
    #define DISPLAY_BENCHMARK_TE_ENABLED 0U
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

typedef enum display_benchmark_load
{
    DISPLAY_BENCHMARK_LOAD_DISPLAY_ONLY = 0,
    DISPLAY_BENCHMARK_LOAD_FULL,
    DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY,
    DISPLAY_BENCHMARK_LOAD_TCP_ONLY,
} display_benchmark_load_t;

#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
_Static_assert(
    CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_FULL +
    CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_AUDIO_ONLY +
    CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_TCP_ONLY == 1,
    "characterization requires exactly one second-stage load profile");
#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_AUDIO_ONLY
#define DISPLAY_BENCHMARK_SELECTED_LOAD \
            DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY
#elif CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_TCP_ONLY
#define DISPLAY_BENCHMARK_SELECTED_LOAD DISPLAY_BENCHMARK_LOAD_TCP_ONLY
#else
#define DISPLAY_BENCHMARK_SELECTED_LOAD DISPLAY_BENCHMARK_LOAD_FULL
#endif
#else
#define DISPLAY_BENCHMARK_SELECTED_LOAD DISPLAY_BENCHMARK_LOAD_FULL
#endif

typedef struct display_benchmark_stability_summary
{
    uint32_t sample_count;
    uint32_t sample_error_count;
    uint32_t lvgl_lock_error_count;
    uint32_t fps_read_error_count;
    uint32_t maximum_fps_lock_wait_us;
    size_t minimum_dma_largest;
    uint32_t dma_failure_count;
    uint32_t frame_submit_count;
    uint32_t submit_failure_count;
    uint32_t transition_cancel_count;
    bool diagnostics_passed;
} display_benchmark_stability_summary_t;

#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
static const app_manager_transition_effect_t s_characterization_effects[
    DISPLAY_BENCHMARK_CHARACTERIZATION_EFFECT_COUNT] =
{
    APP_MANAGER_TRANSITION_FADE,
    APP_MANAGER_TRANSITION_PUSH_LEFT,
    APP_MANAGER_TRANSITION_PUSH_RIGHT,
    APP_MANAGER_TRANSITION_COVER_LEFT,
    APP_MANAGER_TRANSITION_REVEAL_RIGHT,
};
#endif

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
static atomic_bool s_wifi_monitoring_active;
static atomic_bool s_wifi_was_ready;
static atomic_uint s_wifi_disconnect_count;

static void _display_benchmark_record_stability_error(atomic_int *source,
        esp_err_t error);

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
    return atomic_load_explicit(&s_stop_requested, memory_order_acquire) ||
           atomic_load_explicit(&s_load_stop_requested, memory_order_acquire);
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
    int_fast64_t expected = 0;
    (void)atomic_compare_exchange_strong_explicit(
        &s_tcp_disconnected_since_us, &expected, now_us,
        memory_order_relaxed, memory_order_relaxed);
}

static uint64_t _display_benchmark_tcp_mark_connected(int64_t now_us)
{
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
        .sin_port = htons(CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_PORT),
    };
    if (inet_pton(AF_INET, CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_HOST,
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
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_RATE_KBIT_S;
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
                  CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_HOST,
                  (unsigned)CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_PORT,
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

#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
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
#else
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
#endif

static bool _display_benchmark_wifi_ready(void)
{
    wifi_service_status_snapshot_t status = {0};
    return wifi_service_get_status(&status) == ESP_OK &&
           status.state == WIFI_SERVICE_STATE_IP_READY;
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
    if (msg_id != WIFI_SERVICE_MSG ||
            sub_type != WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT ||
            payload == NULL ||
            payload_size != sizeof(wifi_service_status_snapshot_t))
    {
        return;
    }
    wifi_service_status_snapshot_t status;
    memcpy(&status, payload, sizeof(status));
    _display_benchmark_observe_wifi(
        status.state == WIFI_SERVICE_STATE_IP_READY);
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
        (uint64_t)CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_RATE_KBIT_S *
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

static display_benchmark_performance_t _display_benchmark_grade_effect(
    const app_manager_display_effect_benchmark_report_t *effect)
{
    if (effect->transition_start_count == 0U ||
            effect->transition_complete_count !=
            effect->transition_start_count ||
            effect->transition_cancel_count != 0U ||
            effect->active_frame_count == 0U ||
            effect->active_duration_us == 0U ||
            effect->interval_count == 0U)
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

static void _display_benchmark_accumulate_stability(
    display_benchmark_stability_summary_t *summary,
    const app_manager_display_benchmark_report_t *report)
{
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
    if (summary->minimum_dma_largest == 0U ||
            (report->minimum_dma_largest != 0U &&
             report->minimum_dma_largest < summary->minimum_dma_largest))
    {
        summary->minimum_dma_largest = report->minimum_dma_largest;
    }
    summary->dma_failure_count += report->dma_failure_count;
    summary->frame_submit_count += report->frame_submit_count;
    summary->submit_failure_count += report->submit_failure_count;
    summary->transition_cancel_count += report->transition_cancel_count;
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
        LOG_I("display perf load=%s effect=%s result=%s start=%u complete=%u cancel=%u active_frames=%u active_ms=%llu avg_fps_x100=%u intervals=%u target_pct_x100=%u p50_us=%u p95_us=%u p99_us=%u max_us=%u long_run=%u",
              load_name, _display_benchmark_effect_name(effect->effect),
              _display_benchmark_performance_name(
                  _display_benchmark_grade_effect(effect)),
              (unsigned)effect->transition_start_count,
              (unsigned)effect->transition_complete_count,
              (unsigned)effect->transition_cancel_count,
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
    LOG_I("display profile load=%s diagnostics=%s samples=%u sample_err=%u lock_err=%u fps_read_err=%u fps_lock_max_us=%u min_fps=%u fps_below_30=%u min_dma=%u dma_fail=%u frame_submits=%u panel_submits=%u submit_fail=%u",
          load_name, report->passed ? "PASS" : "FAIL",
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
    const bool stability_passed = !aborted && completed &&
                                  workload_error == ESP_OK &&
                                  control_error == ESP_OK &&
                                  audio_error == ESP_OK && tcp_error == ESP_OK &&
                                  atomic_load_explicit(
                                      &s_wifi_disconnect_count,
                                      memory_order_relaxed) == 0U &&
                                  tcp_report->reconnect_count == 0U &&
                                  tcp_report->throughput_passed &&
                                  summary->diagnostics_passed;
    LOG_I("display benchmark stability=%s performance=%s state=%s samples=%u sample_err=%u lock_err=%u fps_read_err=%u fps_lock_max_us=%u min_dma=%u dma_fail=%u frame_submits=%u submit_fail=%u transition_cancel=%u",
          stability_passed ? "PASS" : "FAIL",
          _display_benchmark_performance_name(performance),
          aborted ? "ABORT" : (completed ? "COMPLETE" : "INCOMPLETE"),
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
    LOG_I("display config qspi_hz=%u draw_rows=%u color=%s dma_rows=%u dma_max_full_rows=%u queue=%u direct=%u te=%u draw_units=%u draw_prio=%u tcp_payload=%u tcp_prio=%u load_profile=%s lifecycle_log=%u",
          (unsigned)CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ,
          (unsigned)CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT,
          DISPLAY_BENCHMARK_COLOR_FORMAT,
          (unsigned)CONFIG_BSP_DISPLAY_SPI_MAX_TRANSFER_LINES,
          (unsigned)DISPLAY_BENCHMARK_DMA_MAX_FULL_LINES,
          (unsigned)DISPLAY_BENCHMARK_SPI_QUEUE_DEPTH,
          (unsigned)DISPLAY_BENCHMARK_DIRECT_DMA_ENABLED,
          (unsigned)DISPLAY_BENCHMARK_TE_ENABLED,
          (unsigned)CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT,
          (unsigned)CONFIG_LV_DRAW_THREAD_PRIO,
          (unsigned)DISPLAY_BENCHMARK_TCP_PAYLOAD_BYTES,
          (unsigned)DISPLAY_BENCHMARK_TCP_PRIORITY,
          _display_benchmark_load_name(DISPLAY_BENCHMARK_SELECTED_LOAD),
          (unsigned)CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG);
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
                DISPLAY_BENCHMARK_SELECTED_LOAD))
    {
        _display_benchmark_observe_wifi(_display_benchmark_wifi_ready());
    }
}

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

#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
static esp_err_t _display_benchmark_run_profile(
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
                                    DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US;
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
#else
static esp_err_t _display_benchmark_run_profile(
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
        if (elapsed_us >= DISPLAY_BENCHMARK_DURATION_US)
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
                 DISPLAY_BENCHMARK_DURATION_US;
    return result != ESP_OK ? result : end_result;
}
#endif

static void _display_benchmark_supervisor_task(void *arg)
{
    (void)arg;
    const display_benchmark_load_t load = DISPLAY_BENCHMARK_SELECTED_LOAD;
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
#if CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION
    bool display_only_completed = false;
    memset(report, 0, sizeof(*report));
    esp_err_t result = _display_benchmark_run_profile(
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
                      _display_benchmark_grade_production_effects(report));
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
    result = _display_benchmark_run_profile(&page, report, &load_completed);
    if (result != ESP_OK)
    {
        _display_benchmark_record_error(&s_control_error, result);
        goto exit;
    }
    _display_benchmark_log_profile(load, report);
    _display_benchmark_accumulate_stability(&summary, report);
    performance = _display_benchmark_worst_performance(
                      performance,
                      _display_benchmark_grade_production_effects(report));
    completed = display_only_completed && load_completed;
#else
    esp_err_t result = _display_benchmark_start_load(load, &audio_started,
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
    performance = _display_benchmark_grade_production_effects(report);
#endif

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

esp_err_t display_benchmark_start(void)
{
    if (s_supervisor_task != NULL || s_supervisor_stopped != NULL ||
            s_wifi_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        return ESP_ERR_INVALID_STATE;
    }
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
    esp_err_t result = ESP_OK;
    if (_display_benchmark_load_requires_tcp(
                DISPLAY_BENCHMARK_SELECTED_LOAD))
    {
        result = event_bus_subscribe(
                     WIFI_SERVICE_MSG,
                     WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
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

esp_err_t display_benchmark_start(void)
{
    return ESP_OK;
}

esp_err_t display_benchmark_stop(void)
{
    return ESP_OK;
}

#endif

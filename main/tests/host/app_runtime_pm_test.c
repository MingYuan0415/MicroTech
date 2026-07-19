#include "app_runtime_pm.h"

#include "app_manager.h"
#include "audio_service.h"
#include "bsp_hal.h"
#include "imu_service.h"
#include "power_service.h"
#include "system_pm.h"
#include "time_service.h"
#include "wifi_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define TEST_SLEEP_TIMEOUT_MS  1000U
#define TEST_TRACE_CAPACITY    64U

typedef enum
{
    TEST_CALL_BSP_GET_INPUT = 0,
    TEST_CALL_BSP_GET_WAKE_DESCRIPTOR,
    TEST_CALL_BSP_GET_POWER,
    TEST_CALL_POWER_REGISTER,
    TEST_CALL_SYSTEM_REQUEST,
    TEST_CALL_SYSTEM_CANCEL,
    TEST_CALL_WIFI_SUSPEND,
    TEST_CALL_WIFI_RESUME,
    TEST_CALL_AUDIO_SUSPEND,
    TEST_CALL_AUDIO_RESUME,
    TEST_CALL_IMU_SUSPEND,
    TEST_CALL_IMU_RESUME,
    TEST_CALL_TIME_SUSPEND,
    TEST_CALL_TIME_RESUME,
    TEST_CALL_POWER_SUSPEND,
    TEST_CALL_POWER_RESUME,
    TEST_CALL_INPUT_PREPARE,
    TEST_CALL_INPUT_COMPLETE,
    TEST_CALL_INPUT_REGISTER,
    TEST_CALL_INPUT_UNREGISTER,
    TEST_CALL_PREPARE_GUARD,
    TEST_CALL_COMMIT_GUARD,
    TEST_CALL_COMMIT_CALLBACK,
    TEST_CALL_SYSTEM_WAKE,
    TEST_CALL_COUNT,
} test_call_t;

typedef struct test_result_script
{
    esp_err_t fallback;
    esp_err_t next;
    bool has_next;
} test_result_script_t;

typedef struct test_context
{
    test_call_t trace[TEST_TRACE_CAPACITY];
    size_t trace_count;
    test_result_script_t scripts[TEST_CALL_COUNT];
    bsp_wakeup_descriptor_t wake_descriptor;
    bsp_input_ops_t input_ops;
    bsp_power_ops_t power_ops;
    power_service_power_ops_t registered_power_ops;
    bsp_input_cb_t bsp_input_callback;
    void *bsp_input_context;
    bool expose_input;
    bool expose_power;
    bool power_ops_registered;
    audio_service_state_t audio_state;
    bool prepare_guard;
    bool commit_guard;
    uint32_t commit_generation;
    void *commit_context;
    app_manager_key_t delivered_key;
    app_manager_key_event_t delivered_event;
    void *delivered_context;
    unsigned delivered_count;
} test_context_t;

typedef struct test_standby_thread
{
    app_manager_standby_ops_t ops;
    esp_err_t result;
} test_standby_thread_t;

static test_context_t s_test;
static pthread_mutex_t s_trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_request_condition = PTHREAD_COND_INITIALIZER;
static bool s_block_request;
static bool s_request_entered;
static bool s_request_released;
static atomic_uint s_delay_count = ATOMIC_VAR_INIT(0U);

static void _test_record(test_call_t call)
{
    pthread_mutex_lock(&s_trace_mutex);
    assert(s_test.trace_count < TEST_TRACE_CAPACITY);
    s_test.trace[s_test.trace_count++] = call;
    pthread_mutex_unlock(&s_trace_mutex);
}

static esp_err_t _test_scripted_call(test_call_t call)
{
    _test_record(call);
    test_result_script_t *script = &s_test.scripts[call];
    esp_err_t result = script->fallback;
    if (script->has_next)
    {
        result = script->next;
        script->has_next = false;
    }
    return result;
}

static void _test_fail_once(test_call_t call, esp_err_t result)
{
    s_test.scripts[call].next = result;
    s_test.scripts[call].has_next = true;
}

static void _test_clear_trace(void)
{
    pthread_mutex_lock(&s_trace_mutex);
    s_test.trace_count = 0U;
    pthread_mutex_unlock(&s_trace_mutex);
}

static void _test_expect_trace(const test_call_t *expected, size_t count)
{
    pthread_mutex_lock(&s_trace_mutex);
    assert(s_test.trace_count == count);
    for (size_t index = 0; index < count; ++index)
    {
        assert(s_test.trace[index] == expected[index]);
    }
    pthread_mutex_unlock(&s_trace_mutex);
}

static esp_err_t _test_input_register(bsp_input_cb_t callback, void *context)
{
    esp_err_t result = _test_scripted_call(TEST_CALL_INPUT_REGISTER);
    if (result == ESP_OK)
    {
        assert(callback != NULL);
        assert(s_test.bsp_input_callback == NULL);
        s_test.bsp_input_callback = callback;
        s_test.bsp_input_context = context;
    }
    return result;
}

static esp_err_t _test_input_unregister(void)
{
    esp_err_t result = _test_scripted_call(TEST_CALL_INPUT_UNREGISTER);
    if (result == ESP_OK)
    {
        assert(s_test.bsp_input_callback != NULL);
        s_test.bsp_input_callback = NULL;
        s_test.bsp_input_context = NULL;
    }
    return result;
}

static esp_err_t _test_input_prepare(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_INPUT_PREPARE);
}

static esp_err_t _test_input_complete(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_INPUT_COMPLETE);
}

static bool _test_power_available(void)
{
    return true;
}

static esp_err_t _test_power_get_info(bsp_power_info_t *info)
{
    assert(info != NULL);
    *info = (bsp_power_info_t)
    {
        .battery_voltage_mv = 4012U,
        .battery_percent = 73,
        .is_charging = true,
        .is_vbus_connected = true,
        .is_discharging = false,
        .is_standby = false,
        .is_vbus_good = true,
        .vbus_voltage_mv = 5001U,
        .system_voltage_mv = 3300U,
    };
    return ESP_OK;
}

static void _test_reset(void)
{
    app_runtime_pm_close_admission();
    app_runtime_pm_reset();
    memset(&s_test, 0, sizeof(s_test));
    for (size_t index = 0; index < TEST_CALL_COUNT; ++index)
    {
        s_test.scripts[index].fallback = ESP_OK;
    }
    s_test.wake_descriptor.gpio_mask = UINT64_C(1) << 0;
    s_test.wake_descriptor.active_low_mask = UINT64_C(1) << 0;
    s_test.input_ops = (bsp_input_ops_t)
    {
        .register_handler = _test_input_register,
        .unregister_handler = _test_input_unregister,
        .prepare_sleep = _test_input_prepare,
        .complete_sleep = _test_input_complete,
    };
    s_test.power_ops = (bsp_power_ops_t)
    {
        .is_available = _test_power_available,
        .get_info = _test_power_get_info,
    };
    s_test.expose_input = true;
    s_test.expose_power = true;
    s_test.audio_state = AUDIO_SERVICE_STATE_RUNNING;
    s_test.prepare_guard = true;
    s_test.commit_guard = true;
    pthread_mutex_lock(&s_request_mutex);
    s_block_request = false;
    s_request_entered = false;
    s_request_released = false;
    pthread_mutex_unlock(&s_request_mutex);
    atomic_store(&s_delay_count, 0U);
}

static system_pm_config_t _test_build_config(bool wifi_participant,
        bool imu_participant, bool audio_participant, bool time_participant)
{
    system_pm_config_t config;
    assert(app_runtime_pm_build_system_config(&config) == ESP_OK);
    assert(config.prepare_sleep != NULL);
    assert(config.complete_sleep != NULL);
    assert(config.sleep_hook_context != NULL);
    assert(config.prepare_timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    app_runtime_pm_set_wifi_participant(wifi_participant);
    app_runtime_pm_set_imu_participant(imu_participant);
    app_runtime_pm_set_audio_participant(audio_participant);
    app_runtime_pm_set_time_participant(time_participant);
    return config;
}

static esp_err_t _test_prepare(const system_pm_config_t *config)
{
    return config->prepare_sleep(TEST_SLEEP_TIMEOUT_MS,
                                 config->sleep_hook_context);
}

static esp_err_t _test_complete(const system_pm_config_t *config)
{
    return config->complete_sleep(TEST_SLEEP_TIMEOUT_MS,
                                  config->sleep_hook_context);
}

static void _test_app_input_callback(app_manager_key_t key,
                                     app_manager_key_event_t event,
                                     void *context)
{
    s_test.delivered_key = key;
    s_test.delivered_event = event;
    s_test.delivered_context = context;
    ++s_test.delivered_count;
}

static void *_test_request_thread(void *context)
{
    test_standby_thread_t *thread = context;
    thread->result = thread->ops.request_standby();
    return NULL;
}

static void *_test_close_thread(void *context)
{
    (void)context;
    app_runtime_pm_close_admission();
    return NULL;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host-error";
}

void test_log_write(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
    va_list arguments;
    va_start(arguments, format);
    va_end(arguments);
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    assert(ticks_to_delay == 1U);
    atomic_fetch_add(&s_delay_count, 1U);
    sched_yield();
}

const bsp_input_ops_t *bsp_hal_get_input(void)
{
    _test_record(TEST_CALL_BSP_GET_INPUT);
    return s_test.expose_input ? &s_test.input_ops : NULL;
}

esp_err_t bsp_get_wakeup_descriptor(bsp_wakeup_descriptor_t *descriptor)
{
    assert(descriptor != NULL);
    esp_err_t result = _test_scripted_call(TEST_CALL_BSP_GET_WAKE_DESCRIPTOR);
    if (result == ESP_OK)
    {
        *descriptor = s_test.wake_descriptor;
    }
    return result;
}

const bsp_power_ops_t *bsp_hal_get_power(void)
{
    _test_record(TEST_CALL_BSP_GET_POWER);
    return s_test.expose_power ? &s_test.power_ops : NULL;
}

esp_err_t power_service_register_power_ops(
    const power_service_power_ops_t *ops)
{
    assert(ops != NULL);
    esp_err_t result = _test_scripted_call(TEST_CALL_POWER_REGISTER);
    if (result == ESP_OK)
    {
        s_test.registered_power_ops = *ops;
        s_test.power_ops_registered = true;
    }
    return result;
}

esp_err_t system_pm_request_standby(void)
{
    esp_err_t result = _test_scripted_call(TEST_CALL_SYSTEM_REQUEST);
    pthread_mutex_lock(&s_request_mutex);
    if (s_block_request)
    {
        s_request_entered = true;
        pthread_cond_broadcast(&s_request_condition);
        while (!s_request_released)
        {
            pthread_cond_wait(&s_request_condition, &s_request_mutex);
        }
    }
    pthread_mutex_unlock(&s_request_mutex);
    return result;
}

esp_err_t system_pm_cancel_standby(void)
{
    return _test_scripted_call(TEST_CALL_SYSTEM_CANCEL);
}

esp_err_t wifi_service_suspend(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_WIFI_SUSPEND);
}

esp_err_t wifi_service_resume(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_WIFI_RESUME);
}

esp_err_t audio_service_suspend(uint32_t timeout_ms, bool *resume_required)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    assert(resume_required != NULL);
    *resume_required = s_test.audio_state == AUDIO_SERVICE_STATE_RUNNING;
    esp_err_t result = _test_scripted_call(TEST_CALL_AUDIO_SUSPEND);
    if (result == ESP_OK)
    {
        s_test.audio_state = AUDIO_SERVICE_STATE_READY;
    }
    return result;
}

esp_err_t audio_service_resume(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    esp_err_t result = _test_scripted_call(TEST_CALL_AUDIO_RESUME);
    if (result == ESP_OK)
    {
        s_test.audio_state = AUDIO_SERVICE_STATE_RUNNING;
    }
    return result;
}

esp_err_t imu_service_suspend(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_IMU_SUSPEND);
}

esp_err_t imu_service_resume(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_IMU_RESUME);
}

esp_err_t time_service_suspend(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_TIME_SUSPEND);
}

esp_err_t time_service_resume(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_TIME_RESUME);
}

esp_err_t power_service_suspend(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_POWER_SUSPEND);
}

esp_err_t power_service_resume(uint32_t timeout_ms)
{
    assert(timeout_ms == TEST_SLEEP_TIMEOUT_MS);
    return _test_scripted_call(TEST_CALL_POWER_RESUME);
}

bool app_manager_pm_standby_prepare_guard(void)
{
    _test_record(TEST_CALL_PREPARE_GUARD);
    return s_test.prepare_guard;
}

bool app_manager_pm_standby_commit_guard(uint32_t generation, void *context)
{
    _test_record(TEST_CALL_COMMIT_GUARD);
    s_test.commit_generation = generation;
    s_test.commit_context = context;
    return s_test.commit_guard;
}

void app_manager_pm_standby_commit_callback(uint32_t generation, void *context)
{
    _test_record(TEST_CALL_COMMIT_CALLBACK);
    s_test.commit_generation = generation;
    s_test.commit_context = context;
}

esp_err_t app_manager_pm_notify_system_wake(void)
{
    _test_record(TEST_CALL_SYSTEM_WAKE);
    return ESP_OK;
}

static void _test_config_and_bridges(void)
{
    _test_reset();
    s_test.wake_descriptor.gpio_mask = (UINT64_C(1) << 0) |
                                       (UINT64_C(1) << 9);
    s_test.wake_descriptor.active_low_mask = UINT64_C(1) << 9;
    system_pm_config_t config = _test_build_config(true, true, true, true);
    assert(config.wake_source_count == 2U);
    assert(config.wake_sources[0].gpio_num == 0);
    assert(config.wake_sources[0].active_level == SYSTEM_PM_WAKE_LEVEL_HIGH);
    assert(config.wake_sources[1].gpio_num == 9);
    assert(config.wake_sources[1].active_level == SYSTEM_PM_WAKE_LEVEL_LOW);

    s_test.commit_guard = false;
    assert(!config.commit_guard(47U, config.commit_context));
    assert(s_test.commit_generation == 47U);
    assert(s_test.commit_context == NULL);
    config.commit_callback(48U, config.commit_context);
    assert(s_test.commit_generation == 48U);
    assert(s_test.commit_context == NULL);
    system_pm_wake_event_t event = {0};
    config.wake_callback(&event, config.wake_callback_context);

    app_manager_input_ops_t input = app_runtime_pm_get_input_ops();
    static uint8_t input_context;
    assert(input.register_handler(_test_app_input_callback,
                                  &input_context) == ESP_OK);
    assert(s_test.bsp_input_callback != NULL);
    s_test.bsp_input_callback(BSP_KEY_HOME, BSP_KEY_EVENT_CLICK,
                              s_test.bsp_input_context);
    assert(s_test.delivered_count == 1U);
    assert(s_test.delivered_key == APP_MANAGER_KEY_HOME);
    assert(s_test.delivered_event == APP_MANAGER_KEY_EVENT_CLICK);
    assert(s_test.delivered_context == &input_context);
    s_test.bsp_input_callback(BSP_KEY_POWER, BSP_KEY_EVENT_LONG_PRESS,
                              s_test.bsp_input_context);
    assert(s_test.delivered_count == 2U);
    assert(s_test.delivered_key == APP_MANAGER_KEY_POWER);
    assert(s_test.delivered_event == APP_MANAGER_KEY_EVENT_LONG_PRESS);
    s_test.bsp_input_callback(BSP_KEY_COUNT, BSP_KEY_EVENT_CLICK,
                              s_test.bsp_input_context);
    assert(s_test.delivered_count == 2U);

    _test_fail_once(TEST_CALL_INPUT_UNREGISTER, ESP_FAIL);
    assert(input.unregister_handler() == ESP_FAIL);
    assert(input.register_handler(_test_app_input_callback,
                                  &input_context) == ESP_ERR_INVALID_STATE);
    assert(input.unregister_handler() == ESP_OK);
    assert(s_test.bsp_input_callback == NULL);

    _test_clear_trace();
    assert(app_runtime_pm_prepare_power(BSP_CAPABILITY_POWER) == ESP_OK);
    assert(s_test.power_ops_registered);
    power_info_t info;
    assert(s_test.registered_power_ops.get_info(&info) == ESP_OK);
    assert(info.battery_voltage_mv == 4012U);
    assert(info.battery_percent == 73);
    assert(info.is_charging);
    assert(info.is_vbus_connected);
    app_runtime_pm_clear_power();
    assert(s_test.registered_power_ops.get_info(&info) ==
           ESP_ERR_INVALID_STATE);

    _test_reset();
    s_test.expose_input = false;
    assert(app_runtime_pm_build_system_config(&config) == ESP_ERR_INVALID_STATE);
    _test_reset();
    s_test.wake_descriptor.gpio_mask = 0U;
    assert(app_runtime_pm_build_system_config(&config) == ESP_ERR_INVALID_STATE);
    _test_reset();
    s_test.wake_descriptor.gpio_mask = UINT64_C(0x1F);
    assert(app_runtime_pm_build_system_config(&config) == ESP_ERR_INVALID_SIZE);
}

static void _test_standby_admission(void)
{
    _test_reset();
    app_manager_standby_ops_t ops = app_runtime_pm_get_standby_ops();
    assert(ops.request_standby() == ESP_ERR_INVALID_STATE);
    assert(ops.cancel_standby() == ESP_OK);
    const test_call_t canceled[] = {TEST_CALL_SYSTEM_CANCEL};
    _test_expect_trace(canceled, TEST_ARRAY_SIZE(canceled));

    _test_clear_trace();
    app_runtime_pm_open_admission();
    _test_fail_once(TEST_CALL_SYSTEM_REQUEST, ESP_FAIL);
    assert(ops.request_standby() == ESP_FAIL);
    assert(ops.request_standby() == ESP_OK);
    app_runtime_pm_close_admission();
    assert(ops.request_standby() == ESP_ERR_INVALID_STATE);
    const test_call_t requests[] =
    {
        TEST_CALL_SYSTEM_REQUEST,
        TEST_CALL_SYSTEM_REQUEST,
    };
    _test_expect_trace(requests, TEST_ARRAY_SIZE(requests));
}

static void _test_standby_race(void)
{
    _test_reset();
    test_standby_thread_t request =
    {
        .ops = app_runtime_pm_get_standby_ops(),
        .result = ESP_FAIL,
    };
    app_runtime_pm_open_admission();
    pthread_mutex_lock(&s_request_mutex);
    s_block_request = true;
    pthread_mutex_unlock(&s_request_mutex);

    pthread_t requester;
    assert(pthread_create(&requester, NULL, _test_request_thread, &request) == 0);
    pthread_mutex_lock(&s_request_mutex);
    while (!s_request_entered)
    {
        pthread_cond_wait(&s_request_condition, &s_request_mutex);
    }
    pthread_mutex_unlock(&s_request_mutex);

    pthread_t closer;
    assert(pthread_create(&closer, NULL, _test_close_thread, NULL) == 0);
    while (atomic_load(&s_delay_count) == 0U)
    {
        sched_yield();
    }
    assert(request.ops.request_standby() == ESP_ERR_INVALID_STATE);

    pthread_mutex_lock(&s_request_mutex);
    s_request_released = true;
    pthread_cond_broadcast(&s_request_condition);
    pthread_mutex_unlock(&s_request_mutex);
    assert(pthread_join(requester, NULL) == 0);
    assert(pthread_join(closer, NULL) == 0);
    assert(request.result == ESP_OK);
    const test_call_t expected[] = {TEST_CALL_SYSTEM_REQUEST};
    _test_expect_trace(expected, TEST_ARRAY_SIZE(expected));
}

static void _test_sleep_order(void)
{
    _test_reset();
    system_pm_config_t config = _test_build_config(true, true, true, true);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_OK);
    const test_call_t prepare[] =
    {
        TEST_CALL_WIFI_SUSPEND,
        TEST_CALL_AUDIO_SUSPEND,
        TEST_CALL_IMU_SUSPEND,
        TEST_CALL_TIME_SUSPEND,
        TEST_CALL_POWER_SUSPEND,
        TEST_CALL_INPUT_PREPARE,
        TEST_CALL_PREPARE_GUARD,
    };
    _test_expect_trace(prepare, TEST_ARRAY_SIZE(prepare));

    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    const test_call_t complete[] =
    {
        TEST_CALL_INPUT_COMPLETE,
        TEST_CALL_POWER_RESUME,
        TEST_CALL_TIME_RESUME,
        TEST_CALL_IMU_RESUME,
        TEST_CALL_AUDIO_RESUME,
        TEST_CALL_WIFI_RESUME,
    };
    _test_expect_trace(complete, TEST_ARRAY_SIZE(complete));
    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    _test_expect_trace(NULL, 0U);

    _test_reset();
    config = _test_build_config(false, false, false, false);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_OK);
    const test_call_t offline_prepare[] =
    {
        TEST_CALL_POWER_SUSPEND,
        TEST_CALL_INPUT_PREPARE,
        TEST_CALL_PREPARE_GUARD,
    };
    _test_expect_trace(offline_prepare, TEST_ARRAY_SIZE(offline_prepare));
    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    const test_call_t offline_complete[] =
    {
        TEST_CALL_INPUT_COMPLETE,
        TEST_CALL_POWER_RESUME,
    };
    _test_expect_trace(offline_complete, TEST_ARRAY_SIZE(offline_complete));
}

static void _test_ready_audio_is_not_resumed(void)
{
    _test_reset();
    s_test.audio_state = AUDIO_SERVICE_STATE_READY;
    system_pm_config_t config = _test_build_config(true, true, true, true);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_OK);
    const test_call_t prepare[] =
    {
        TEST_CALL_WIFI_SUSPEND,
        TEST_CALL_AUDIO_SUSPEND,
        TEST_CALL_IMU_SUSPEND,
        TEST_CALL_TIME_SUSPEND,
        TEST_CALL_POWER_SUSPEND,
        TEST_CALL_INPUT_PREPARE,
        TEST_CALL_PREPARE_GUARD,
    };
    _test_expect_trace(prepare, TEST_ARRAY_SIZE(prepare));

    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    const test_call_t complete[] =
    {
        TEST_CALL_INPUT_COMPLETE,
        TEST_CALL_POWER_RESUME,
        TEST_CALL_TIME_RESUME,
        TEST_CALL_IMU_RESUME,
        TEST_CALL_WIFI_RESUME,
    };
    _test_expect_trace(complete, TEST_ARRAY_SIZE(complete));
    assert(s_test.audio_state == AUDIO_SERVICE_STATE_READY);
}

static void _test_error_audio_must_be_stopped(void)
{
    _test_reset();
    s_test.audio_state = AUDIO_SERVICE_STATE_ERROR;
    system_pm_config_t config = _test_build_config(true, true, true, true);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_OK);
    const test_call_t prepare[] =
    {
        TEST_CALL_WIFI_SUSPEND,
        TEST_CALL_AUDIO_SUSPEND,
        TEST_CALL_IMU_SUSPEND,
        TEST_CALL_TIME_SUSPEND,
        TEST_CALL_POWER_SUSPEND,
        TEST_CALL_INPUT_PREPARE,
        TEST_CALL_PREPARE_GUARD,
    };
    _test_expect_trace(prepare, TEST_ARRAY_SIZE(prepare));

    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    const test_call_t complete[] =
    {
        TEST_CALL_INPUT_COMPLETE,
        TEST_CALL_POWER_RESUME,
        TEST_CALL_TIME_RESUME,
        TEST_CALL_IMU_RESUME,
        TEST_CALL_WIFI_RESUME,
    };
    _test_expect_trace(complete, TEST_ARRAY_SIZE(complete));
    assert(s_test.audio_state == AUDIO_SERVICE_STATE_READY);

    _test_reset();
    s_test.audio_state = AUDIO_SERVICE_STATE_ERROR;
    config = _test_build_config(true, true, true, true);
    _test_fail_once(TEST_CALL_AUDIO_SUSPEND, ESP_ERR_TIMEOUT);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_ERR_TIMEOUT);
    const test_call_t failed[] =
    {
        TEST_CALL_WIFI_SUSPEND,
        TEST_CALL_AUDIO_SUSPEND,
        TEST_CALL_WIFI_RESUME,
    };
    _test_expect_trace(failed, TEST_ARRAY_SIZE(failed));
    assert(s_test.audio_state == AUDIO_SERVICE_STATE_ERROR);

    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    _test_expect_trace(NULL, 0U);

    _test_reset();
    s_test.audio_state = AUDIO_SERVICE_STATE_SUSPENDING;
    config = _test_build_config(true, true, true, true);
    _test_fail_once(TEST_CALL_AUDIO_SUSPEND, ESP_ERR_INVALID_STATE);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_ERR_INVALID_STATE);
    _test_expect_trace(failed, TEST_ARRAY_SIZE(failed));
    assert(s_test.audio_state == AUDIO_SERVICE_STATE_SUSPENDING);
}

static void _test_sleep_prepare_failures(void)
{
    static const struct
    {
        test_call_t failure_call;
        esp_err_t expected_result;
        test_call_t expected[13];
        size_t expected_count;
        bool reject_guard;
    } cases[] =
    {
        {
            TEST_CALL_WIFI_SUSPEND,
            ESP_ERR_TIMEOUT,
            {TEST_CALL_WIFI_SUSPEND, TEST_CALL_WIFI_RESUME},
            2U,
            false,
        },
        {
            TEST_CALL_AUDIO_SUSPEND,
            ESP_ERR_TIMEOUT,
            {
                TEST_CALL_WIFI_SUSPEND, TEST_CALL_AUDIO_SUSPEND,
                TEST_CALL_AUDIO_RESUME,
                TEST_CALL_WIFI_RESUME
            },
            4U,
            false,
        },
        {
            TEST_CALL_IMU_SUSPEND,
            ESP_ERR_TIMEOUT,
            {
                TEST_CALL_WIFI_SUSPEND, TEST_CALL_AUDIO_SUSPEND,
                TEST_CALL_IMU_SUSPEND, TEST_CALL_IMU_RESUME,
                TEST_CALL_AUDIO_RESUME,
                TEST_CALL_WIFI_RESUME
            },
            6U,
            false,
        },
        {
            TEST_CALL_TIME_SUSPEND,
            ESP_ERR_TIMEOUT,
            {
                TEST_CALL_WIFI_SUSPEND, TEST_CALL_AUDIO_SUSPEND,
                TEST_CALL_IMU_SUSPEND, TEST_CALL_TIME_SUSPEND,
                TEST_CALL_TIME_RESUME, TEST_CALL_IMU_RESUME,
                TEST_CALL_AUDIO_RESUME, TEST_CALL_WIFI_RESUME
            },
            8U,
            false,
        },
        {
            TEST_CALL_POWER_SUSPEND,
            ESP_ERR_TIMEOUT,
            {
                TEST_CALL_WIFI_SUSPEND, TEST_CALL_AUDIO_SUSPEND,
                TEST_CALL_IMU_SUSPEND,
                TEST_CALL_TIME_SUSPEND, TEST_CALL_POWER_SUSPEND,
                TEST_CALL_POWER_RESUME, TEST_CALL_TIME_RESUME,
                TEST_CALL_IMU_RESUME, TEST_CALL_AUDIO_RESUME,
                TEST_CALL_WIFI_RESUME
            },
            10U,
            false,
        },
        {
            TEST_CALL_INPUT_PREPARE,
            ESP_ERR_TIMEOUT,
            {
                TEST_CALL_WIFI_SUSPEND, TEST_CALL_AUDIO_SUSPEND,
                TEST_CALL_IMU_SUSPEND,
                TEST_CALL_TIME_SUSPEND, TEST_CALL_POWER_SUSPEND,
                TEST_CALL_INPUT_PREPARE, TEST_CALL_INPUT_COMPLETE,
                TEST_CALL_POWER_RESUME, TEST_CALL_TIME_RESUME,
                TEST_CALL_IMU_RESUME, TEST_CALL_AUDIO_RESUME,
                TEST_CALL_WIFI_RESUME
            },
            12U,
            false,
        },
        {
            TEST_CALL_PREPARE_GUARD,
            ESP_ERR_INVALID_STATE,
            {
                TEST_CALL_WIFI_SUSPEND, TEST_CALL_AUDIO_SUSPEND,
                TEST_CALL_IMU_SUSPEND,
                TEST_CALL_TIME_SUSPEND, TEST_CALL_POWER_SUSPEND,
                TEST_CALL_INPUT_PREPARE,
                TEST_CALL_PREPARE_GUARD, TEST_CALL_INPUT_COMPLETE,
                TEST_CALL_POWER_RESUME, TEST_CALL_TIME_RESUME,
                TEST_CALL_IMU_RESUME,
                TEST_CALL_AUDIO_RESUME, TEST_CALL_WIFI_RESUME
            },
            13U,
            true,
        },
    };

    for (size_t index = 0; index < TEST_ARRAY_SIZE(cases); ++index)
    {
        _test_reset();
        system_pm_config_t config = _test_build_config(true, true, true, true);
        if (cases[index].reject_guard)
        {
            s_test.prepare_guard = false;
        }
        else
        {
            _test_fail_once(cases[index].failure_call, ESP_ERR_TIMEOUT);
        }
        _test_clear_trace();
        assert(_test_prepare(&config) == cases[index].expected_result);
        _test_expect_trace(cases[index].expected,
                           cases[index].expected_count);
        _test_clear_trace();
        assert(_test_complete(&config) == ESP_OK);
        _test_expect_trace(NULL, 0U);
    }
}

static void _test_sleep_recovery_matrix(void)
{
    static const test_call_t recovery_calls[] =
    {
        TEST_CALL_INPUT_COMPLETE,
        TEST_CALL_POWER_RESUME,
        TEST_CALL_TIME_RESUME,
        TEST_CALL_IMU_RESUME,
        TEST_CALL_AUDIO_RESUME,
        TEST_CALL_WIFI_RESUME,
    };
    static const esp_err_t errors[] =
    {
        ESP_ERR_INVALID_STATE,
        ESP_ERR_TIMEOUT,
        ESP_FAIL,
        ESP_ERR_NO_MEM,
        ESP_ERR_INVALID_ARG,
        ESP_ERR_INVALID_SIZE,
    };

    for (unsigned mask = 0U;
            mask < (1U << TEST_ARRAY_SIZE(recovery_calls)); ++mask)
    {
        _test_reset();
        system_pm_config_t config = _test_build_config(true, true, true, true);
        _test_clear_trace();
        assert(_test_prepare(&config) == ESP_OK);
        for (size_t bit = 0; bit < TEST_ARRAY_SIZE(recovery_calls); ++bit)
        {
            if ((mask & (1U << bit)) != 0U)
            {
                _test_fail_once(recovery_calls[bit], errors[bit]);
            }
        }

        esp_err_t expected_result = ESP_OK;
        for (size_t bit = 0; bit < TEST_ARRAY_SIZE(errors); ++bit)
        {
            if ((mask & (1U << bit)) != 0U)
            {
                expected_result = errors[bit];
                break;
            }
        }
        _test_clear_trace();
        assert(_test_complete(&config) == expected_result);
        _test_expect_trace(recovery_calls,
                           TEST_ARRAY_SIZE(recovery_calls));

        test_call_t retry[TEST_ARRAY_SIZE(recovery_calls)];
        size_t retry_count = 0U;
        for (size_t bit = 0; bit < TEST_ARRAY_SIZE(recovery_calls); ++bit)
        {
            if ((mask & (1U << bit)) != 0U)
            {
                retry[retry_count++] = recovery_calls[bit];
            }
        }
        _test_clear_trace();
        assert(_test_complete(&config) == ESP_OK);
        _test_expect_trace(retry, retry_count);
    }
}

static void _test_pending_recovery_before_prepare(void)
{
    _test_reset();
    system_pm_config_t config = _test_build_config(true, true, true, true);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_OK);
    _test_fail_once(TEST_CALL_INPUT_COMPLETE, ESP_ERR_TIMEOUT);
    _test_clear_trace();
    assert(_test_complete(&config) == ESP_ERR_TIMEOUT);

    _test_fail_once(TEST_CALL_INPUT_COMPLETE, ESP_FAIL);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_FAIL);
    const test_call_t blocked[] = {TEST_CALL_INPUT_COMPLETE};
    _test_expect_trace(blocked, TEST_ARRAY_SIZE(blocked));

    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_OK);
    const test_call_t recovered[] =
    {
        TEST_CALL_INPUT_COMPLETE,
        TEST_CALL_WIFI_SUSPEND,
        TEST_CALL_AUDIO_SUSPEND,
        TEST_CALL_IMU_SUSPEND,
        TEST_CALL_TIME_SUSPEND,
        TEST_CALL_POWER_SUSPEND,
        TEST_CALL_INPUT_PREPARE,
        TEST_CALL_PREPARE_GUARD,
    };
    _test_expect_trace(recovered, TEST_ARRAY_SIZE(recovered));
    assert(_test_complete(&config) == ESP_OK);

    _test_reset();
    config = _test_build_config(true, true, true, true);
    _test_fail_once(TEST_CALL_WIFI_SUSPEND, ESP_FAIL);
    _test_fail_once(TEST_CALL_WIFI_RESUME, ESP_ERR_TIMEOUT);
    _test_clear_trace();
    assert(_test_prepare(&config) == ESP_FAIL);
    const test_call_t initial[] =
    {
        TEST_CALL_WIFI_SUSPEND,
        TEST_CALL_WIFI_RESUME,
    };
    _test_expect_trace(initial, TEST_ARRAY_SIZE(initial));
    _test_clear_trace();
    assert(_test_complete(&config) == ESP_OK);
    const test_call_t retry[] = {TEST_CALL_WIFI_RESUME};
    _test_expect_trace(retry, TEST_ARRAY_SIZE(retry));
}

int main(void)
{
    _test_config_and_bridges();
    _test_standby_admission();
    _test_standby_race();
    _test_sleep_order();
    _test_ready_audio_is_not_resumed();
    _test_error_audio_must_be_stopped();
    _test_sleep_prepare_failures();
    _test_sleep_recovery_matrix();
    _test_pending_recovery_before_prepare();
    app_runtime_pm_close_admission();
    app_runtime_pm_reset();
    return 0;
}

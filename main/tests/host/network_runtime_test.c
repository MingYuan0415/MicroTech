#include "network_runtime.h"

#include "esp_event.h"
#include "esp_netif.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEST_RESULT_CAPACITY 8U
#define TEST_IDEMPOTENT_CALLS 8U

typedef struct result_script
{
    esp_err_t results[TEST_RESULT_CAPACITY];
    size_t result_count;
    size_t result_index;
    esp_err_t fallback;
    unsigned calls;
} result_script_t;

typedef struct init_thread_context
{
    esp_err_t result;
} init_thread_context_t;

static pthread_mutex_t s_fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_fake_changed = PTHREAD_COND_INITIALIZER;
static result_script_t s_netif_script;
static result_script_t s_event_loop_script;
static bool s_block_netif;
static bool s_netif_entered;

static void _test_script_set(result_script_t *script, esp_err_t fallback,
                             const esp_err_t *results, size_t result_count)
{
    assert(result_count <= TEST_RESULT_CAPACITY);
    memset(script, 0, sizeof(*script));
    script->fallback = fallback;
    if (result_count > 0U)
    {
        memcpy(script->results, results,
               result_count * sizeof(results[0]));
    }
    script->result_count = result_count;
}

static esp_err_t _test_script_next_locked(result_script_t *script)
{
    esp_err_t result = script->fallback;
    script->calls++;
    if (script->result_index < script->result_count)
    {
        result = script->results[script->result_index++];
    }
    return result;
}

static void _test_fake_reset(void)
{
    int result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    _test_script_set(&s_netif_script, ESP_OK, NULL, 0U);
    _test_script_set(&s_event_loop_script, ESP_OK, NULL, 0U);
    s_block_netif = false;
    s_netif_entered = false;
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);
}

static void _test_fake_set_scripts(const esp_err_t *netif_results,
                                   size_t netif_count,
                                   esp_err_t netif_fallback,
                                   const esp_err_t *event_results,
                                   size_t event_count,
                                   esp_err_t event_fallback)
{
    int result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    _test_script_set(&s_netif_script, netif_fallback,
                     netif_results, netif_count);
    _test_script_set(&s_event_loop_script, event_fallback,
                     event_results, event_count);
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);
}

static unsigned _test_fake_calls(const result_script_t *script)
{
    int result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    unsigned calls = script->calls;
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);
    return calls;
}

esp_err_t esp_netif_init(void)
{
    int result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    esp_err_t api_result = _test_script_next_locked(&s_netif_script);
    s_netif_entered = true;
    result = pthread_cond_broadcast(&s_fake_changed);
    assert(result == 0);
    while (s_block_netif)
    {
        result = pthread_cond_wait(&s_fake_changed, &s_fake_lock);
        assert(result == 0);
    }
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);
    return api_result;
}

esp_err_t esp_event_loop_create_default(void)
{
    int result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    esp_err_t api_result = _test_script_next_locked(&s_event_loop_script);
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);
    return api_result;
}

static void _test_assert_status(network_runtime_resource_state_t netif,
                                network_runtime_resource_state_t event_loop,
                                bool ready)
{
    network_runtime_status_t status = network_runtime_get_status();
    assert(status.netif == netif);
    assert(status.default_event_loop == event_loop);
    assert(status.ready == ready);
    assert(network_runtime_is_ready() == ready);
}

static void _test_assert_idempotent(unsigned netif_calls,
                                    unsigned event_loop_calls)
{
    for (size_t index = 0; index < TEST_IDEMPOTENT_CALLS; index++)
    {
        assert(network_runtime_init() == ESP_OK);
    }
    assert(_test_fake_calls(&s_netif_script) == netif_calls);
    assert(_test_fake_calls(&s_event_loop_script) == event_loop_calls);
}

static void _test_owned(void)
{
    _test_fake_reset();
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_UNINITIALIZED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);
    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_OWNED, true);
    _test_assert_idempotent(1U, 1U);
}

static void _test_shared(void)
{
    _test_fake_reset();
    const esp_err_t shared[] = {ESP_ERR_INVALID_STATE};
    _test_fake_set_scripts(shared, 1U, ESP_FAIL, shared, 1U, ESP_FAIL);
    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_SHARED,
                        NETWORK_RUNTIME_RESOURCE_SHARED, true);
    _test_assert_idempotent(1U, 1U);
}

static void _test_owned_netif_shared_loop(void)
{
    _test_fake_reset();
    const esp_err_t event_results[] = {ESP_ERR_INVALID_STATE};
    _test_fake_set_scripts(NULL, 0U, ESP_OK,
                           event_results, 1U, ESP_FAIL);
    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_SHARED, true);
    _test_assert_idempotent(1U, 1U);
}

static void _test_shared_netif_owned_loop(void)
{
    _test_fake_reset();
    const esp_err_t netif_results[] = {ESP_ERR_INVALID_STATE};
    _test_fake_set_scripts(netif_results, 1U, ESP_FAIL,
                           NULL, 0U, ESP_OK);
    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_SHARED,
                        NETWORK_RUNTIME_RESOURCE_OWNED, true);
    _test_assert_idempotent(1U, 1U);
}

static void _test_netif_failure_retry(void)
{
    _test_fake_reset();
    const esp_err_t netif_results[] =
    {
        ESP_FAIL,
        ESP_ERR_NO_MEM,
        ESP_OK,
    };
    _test_fake_set_scripts(netif_results, 3U, ESP_FAIL,
                           NULL, 0U, ESP_OK);

    assert(network_runtime_init() == ESP_FAIL);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_UNINITIALIZED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);
    assert(_test_fake_calls(&s_event_loop_script) == 0U);

    assert(network_runtime_init() == ESP_ERR_NO_MEM);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_UNINITIALIZED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);
    assert(_test_fake_calls(&s_event_loop_script) == 0U);

    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_OWNED, true);
    _test_assert_idempotent(3U, 1U);
}

static void _test_event_loop_failure_retry(void)
{
    _test_fake_reset();
    const esp_err_t event_results[] =
    {
        ESP_ERR_NO_MEM,
        ESP_FAIL,
        ESP_OK,
    };
    _test_fake_set_scripts(NULL, 0U, ESP_OK,
                           event_results, 3U, ESP_FAIL);

    assert(network_runtime_init() == ESP_ERR_NO_MEM);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);
    assert(_test_fake_calls(&s_netif_script) == 1U);

    assert(network_runtime_init() == ESP_FAIL);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);
    assert(_test_fake_calls(&s_netif_script) == 1U);

    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_OWNED, true);
    _test_assert_idempotent(1U, 3U);
}

static void _test_shared_netif_event_loop_retry(void)
{
    _test_fake_reset();
    const esp_err_t netif_results[] = {ESP_ERR_INVALID_STATE};
    const esp_err_t event_results[] =
    {
        ESP_FAIL,
        ESP_ERR_INVALID_STATE,
    };
    _test_fake_set_scripts(netif_results, 1U, ESP_FAIL,
                           event_results, 2U, ESP_FAIL);

    assert(network_runtime_init() == ESP_FAIL);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_SHARED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);
    assert(_test_fake_calls(&s_netif_script) == 1U);

    assert(network_runtime_init() == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_SHARED,
                        NETWORK_RUNTIME_RESOURCE_SHARED, true);
    _test_assert_idempotent(1U, 2U);
}

static void *_test_init_thread(void *argument)
{
    init_thread_context_t *context = argument;
    context->result = network_runtime_init();
    return NULL;
}

static void _test_concurrent_init(void)
{
    _test_fake_reset();
    int result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    s_block_netif = true;
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);

    init_thread_context_t first = {.result = ESP_FAIL};
    pthread_t first_thread;
    result = pthread_create(&first_thread, NULL, _test_init_thread, &first);
    assert(result == 0);

    result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    while (!s_netif_entered)
    {
        result = pthread_cond_wait(&s_fake_changed, &s_fake_lock);
        assert(result == 0);
    }
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);

    assert(network_runtime_init() == ESP_ERR_INVALID_STATE);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_UNINITIALIZED,
                        NETWORK_RUNTIME_RESOURCE_UNINITIALIZED, false);

    result = pthread_mutex_lock(&s_fake_lock);
    assert(result == 0);
    s_block_netif = false;
    result = pthread_cond_broadcast(&s_fake_changed);
    assert(result == 0);
    result = pthread_mutex_unlock(&s_fake_lock);
    assert(result == 0);

    result = pthread_join(first_thread, NULL);
    assert(result == 0);
    assert(first.result == ESP_OK);
    _test_assert_status(NETWORK_RUNTIME_RESOURCE_OWNED,
                        NETWORK_RUNTIME_RESOURCE_OWNED, true);

    init_thread_context_t contexts[TEST_IDEMPOTENT_CALLS];
    pthread_t threads[TEST_IDEMPOTENT_CALLS];
    for (size_t index = 0; index < TEST_IDEMPOTENT_CALLS; index++)
    {
        contexts[index].result = ESP_FAIL;
        result = pthread_create(&threads[index], NULL,
                                _test_init_thread, &contexts[index]);
        assert(result == 0);
    }
    for (size_t index = 0; index < TEST_IDEMPOTENT_CALLS; index++)
    {
        result = pthread_join(threads[index], NULL);
        assert(result == 0);
        assert(contexts[index].result == ESP_OK);
    }
    assert(_test_fake_calls(&s_netif_script) == 1U);
    assert(_test_fake_calls(&s_event_loop_script) == 1U);
}

typedef void (*test_fn_t)(void);

typedef struct test_case
{
    const char *name;
    test_fn_t run;
} test_case_t;

static const test_case_t s_tests[] =
{
    {"owned", _test_owned},
    {"shared", _test_shared},
    {"owned-netif-shared-loop", _test_owned_netif_shared_loop},
    {"shared-netif-owned-loop", _test_shared_netif_owned_loop},
    {"netif-failure-retry", _test_netif_failure_retry},
    {"event-loop-failure-retry", _test_event_loop_failure_retry},
    {"shared-netif-event-loop-retry", _test_shared_netif_event_loop_retry},
    {"concurrent-init", _test_concurrent_init},
};

int main(int argc, char **argv)
{
    int result = 2;
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        goto exit;
    }

    for (size_t index = 0; index < sizeof(s_tests) / sizeof(s_tests[0]);
            index++)
    {
        if (strcmp(argv[1], s_tests[index].name) == 0)
        {
            s_tests[index].run();
            printf("network runtime scenario passed: %s\n", argv[1]);
            result = 0;
            goto exit;
        }
    }
    fprintf(stderr, "unknown scenario: %s\n", argv[1]);

exit:
    return result;
}

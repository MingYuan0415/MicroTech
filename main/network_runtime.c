#include "network_runtime.h"

#include "esp_event.h"
#include "esp_netif.h"

#include <stdatomic.h>

static atomic_int s_netif_state =
    ATOMIC_VAR_INIT(NETWORK_RUNTIME_RESOURCE_UNINITIALIZED);
static atomic_int s_event_loop_state =
    ATOMIC_VAR_INIT(NETWORK_RUNTIME_RESOURCE_UNINITIALIZED);
static atomic_flag s_init_busy = ATOMIC_FLAG_INIT;

static bool _network_runtime_resource_ready(
    network_runtime_resource_state_t state)
{
    return state == NETWORK_RUNTIME_RESOURCE_OWNED ||
           state == NETWORK_RUNTIME_RESOURCE_SHARED;
}

network_runtime_status_t network_runtime_get_status(void)
{
    /* Read in reverse initialization order to preserve snapshot invariants. */
    network_runtime_resource_state_t event_loop =
        (network_runtime_resource_state_t)atomic_load_explicit(
            &s_event_loop_state, memory_order_acquire);
    network_runtime_resource_state_t netif =
        (network_runtime_resource_state_t)atomic_load_explicit(
            &s_netif_state, memory_order_acquire);

    return (network_runtime_status_t)
    {
        .netif = netif,
        .default_event_loop = event_loop,
        .ready = _network_runtime_resource_ready(netif) &&
                 _network_runtime_resource_ready(event_loop),
    };
}

bool network_runtime_is_ready(void)
{
    return network_runtime_get_status().ready;
}

static network_runtime_resource_state_t _network_runtime_result_state(
    esp_err_t result)
{
    return result == ESP_OK ? NETWORK_RUNTIME_RESOURCE_OWNED :
           NETWORK_RUNTIME_RESOURCE_SHARED;
}

esp_err_t network_runtime_init(void)
{
    esp_err_t result = ESP_OK;
    if (network_runtime_is_ready())
    {
        return result;
    }

    if (atomic_flag_test_and_set_explicit(&s_init_busy,
                                          memory_order_acquire))
    {
        result = network_runtime_is_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
        return result;
    }

    network_runtime_status_t status = network_runtime_get_status();
    if (!_network_runtime_resource_ready(status.netif))
    {
        result = esp_netif_init();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            goto exit;
        }
        atomic_store_explicit(
            &s_netif_state, _network_runtime_result_state(result),
            memory_order_release);
    }

    status = network_runtime_get_status();
    if (!_network_runtime_resource_ready(status.default_event_loop))
    {
        result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            goto exit;
        }
        atomic_store_explicit(
            &s_event_loop_state, _network_runtime_result_state(result),
            memory_order_release);
    }
    result = ESP_OK;

exit:
    atomic_flag_clear_explicit(&s_init_busy, memory_order_release);
    return result;
}

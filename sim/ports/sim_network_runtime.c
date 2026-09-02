/** @file network_runtime stand-in: IDF netif/event-loop bootstrap is a
 *  hardware domain; the simulated network foundation is always ready so
 *  connectivity_manager (and the setup flow) run their real logic.
 */
#include <stdbool.h>

#include "esp_err.h"
#include "network_runtime.h"

esp_err_t network_runtime_init(void)
{
    return ESP_OK;
}

bool network_runtime_is_ready(void)
{
    return true;
}

network_runtime_status_t network_runtime_get_status(void)
{
    return (network_runtime_status_t)
    {
        .netif = NETWORK_RUNTIME_RESOURCE_OWNED,
        .default_event_loop = NETWORK_RUNTIME_RESOURCE_OWNED,
    };
}

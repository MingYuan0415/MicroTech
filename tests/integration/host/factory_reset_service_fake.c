#include "factory_reset_service.h"
#include "host_factory_reset_service.h"

#include <stdatomic.h>

static atomic_uint s_request_count;

void host_factory_reset_service_reset(void)
{
    atomic_store_explicit(&s_request_count, 0U, memory_order_release);
}

unsigned host_factory_reset_service_request_count(void)
{
    return atomic_load_explicit(&s_request_count, memory_order_acquire);
}

esp_err_t factory_reset_service_request(void)
{
    atomic_fetch_add_explicit(&s_request_count, 1U, memory_order_relaxed);
    return ESP_OK;
}

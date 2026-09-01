#include "chore_service.h"

#include <string.h>

esp_err_t chore_service_submit(const chore_service_job_t *job,
                               chore_service_handle_t *handle)
{
    if (job == NULL || job->run == NULL || handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    static const atomic_bool cancelled = ATOMIC_VAR_INIT(false);
    const chore_service_cancel_token_t token =
    {
        .requested = &cancelled,
    };
    job->run(&token, job->arg);
    if (job->release != NULL)
    {
        job->release(job->arg);
    }
    memset(handle, 0, sizeof(*handle));
    return ESP_OK;
}

esp_err_t chore_service_cancel(chore_service_handle_t *handle,
                               uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(handle, 0, sizeof(*handle));
    return ESP_OK;
}

bool chore_service_cancel_pending(const chore_service_cancel_token_t *cancel)
{
    return cancel != NULL && cancel->requested != NULL &&
           atomic_load_explicit(cancel->requested, memory_order_acquire);
}

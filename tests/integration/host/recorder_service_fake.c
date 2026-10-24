#include "file_service.h"
#include "recorder_service.h"

#include <stddef.h>

static recorder_service_snapshot_t s_snapshot;

esp_err_t recorder_service_init(const recorder_service_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t recorder_service_deinit(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_get_snapshot(recorder_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_snapshot;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_list(recorder_service_file_t *files,
                                size_t capacity, size_t *count)
{
    (void)files;
    (void)capacity;
    if (count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    return ESP_OK;
}

bool recorder_service_is_busy(void)
{
    return false;
}

esp_err_t recorder_service_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t recorder_service_pause(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_resume(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_stop(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_play(const recorder_service_target_t *target)
{
    (void)target;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t recorder_service_stop_playback(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t recorder_service_delete(const recorder_service_target_t *target)
{
    (void)target;
    return ESP_ERR_NOT_SUPPORTED;
}

/* ── File service stand-in ───────────────────────────────────────────────
 * The cross-layer suite exercises page lifecycles, not storage, so the file
 * service reports an unmounted volume with unknown capacity and accepts no
 * destructive action. */

static file_service_result_t s_storage_result;

esp_err_t file_service_init(const file_service_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t file_service_deinit(void)
{
    return ESP_OK;
}

esp_err_t file_service_suspend(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t file_service_resume(void)
{
    return ESP_OK;
}

esp_err_t file_service_get_snapshot(file_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = (file_service_snapshot_t)
    {
        .volume = FILE_SERVICE_VOLUME_UNAVAILABLE,
        .space = { .valid = false, .last_error = ESP_ERR_INVALID_STATE },
        .storage = s_storage_result,
    };
    return ESP_OK;
}

bool file_service_is_busy(void)
{
    return false;
}

esp_err_t file_service_request_storage(file_service_storage_action_t action,
                                       uint32_t expected_generation, file_service_result_t *result)
{
    s_storage_result = (file_service_result_t)
    {
        .state = FILE_SERVICE_RESULT_FAILED,
        .error = ESP_ERR_INVALID_STATE,
        .session = 0U,
    };
    (void)action;
    (void)expected_generation;
    if (result != NULL)
    {
        *result = s_storage_result;
    }
    return ESP_OK;
}

esp_err_t file_service_session_open(uint32_t *session)
{
    if (session == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *session = 0U;
    return ESP_ERR_NO_MEM;
}

esp_err_t file_service_session_close(uint32_t session)
{
    (void)session;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_list(uint32_t session, const char *path,
                                    const file_service_cursor_t *cursor,
                                    file_service_result_t *result)
{
    (void)session;
    (void)path;
    (void)cursor;
    (void)result;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_detail(uint32_t session, const file_service_target_t *target,
                                      file_service_result_t *result)
{
    (void)session;
    (void)target;
    (void)result;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_delete(uint32_t session, const file_service_target_t *target,
                                      file_service_result_t *result)
{
    (void)session;
    (void)target;
    (void)result;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_cancel(uint32_t session)
{
    (void)session;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_get_result(uint32_t session,
        file_service_result_t *result)
{
    (void)session;
    (void)result;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_get_batch(uint32_t session,
        file_service_batch_t *batch)
{
    (void)session;
    (void)batch;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_get_detail(uint32_t session,
        file_service_detail_t *detail)
{
    (void)session;
    (void)detail;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t file_service_session_get_cursor(uint32_t session,
        file_service_cursor_t *cursor)
{
    (void)session;
    (void)cursor;
    return ESP_ERR_INVALID_STATE;
}

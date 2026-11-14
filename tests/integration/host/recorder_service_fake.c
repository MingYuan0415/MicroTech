#include "file_service.h"
#include "recorder_service.h"

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

typedef struct host_recorder_fake
{
    pthread_mutex_t lock;
    recorder_service_snapshot_t snapshot;
    bool available;
    bool fail_next_command;
    esp_err_t next_command_result;
    unsigned command_count;
    unsigned start_count;
    unsigned pause_count;
    unsigned resume_count;
    unsigned stop_count;
} host_recorder_fake_t;

static host_recorder_fake_t s_recorder =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _lock_recorder(void)
{
    assert(pthread_mutex_lock(&s_recorder.lock) == 0);
}

static void _unlock_recorder(void)
{
    assert(pthread_mutex_unlock(&s_recorder.lock) == 0);
}

static esp_err_t _admit_command(unsigned *counter)
{
    _lock_recorder();
    ++s_recorder.command_count;
    ++*counter;
    esp_err_t result = ESP_OK;
    if (!s_recorder.available || s_recorder.snapshot.operation_pending)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_recorder.fail_next_command)
    {
        result = s_recorder.next_command_result;
        s_recorder.fail_next_command = false;
        s_recorder.next_command_result = ESP_OK;
    }
    else
    {
        s_recorder.snapshot.operation_pending = true;
        s_recorder.snapshot.operation_id++;
        s_recorder.snapshot.generation++;
    }
    _unlock_recorder();
    return result;
}

void host_recorder_reset(void)
{
    _lock_recorder();
    memset(&s_recorder.snapshot, 0, sizeof(s_recorder.snapshot));
    s_recorder.available = false;
    s_recorder.fail_next_command = false;
    s_recorder.next_command_result = ESP_OK;
    s_recorder.command_count = 0U;
    s_recorder.start_count = 0U;
    s_recorder.pause_count = 0U;
    s_recorder.resume_count = 0U;
    s_recorder.stop_count = 0U;
    _unlock_recorder();
}

void host_recorder_seed_recording(uint32_t duration_ms)
{
    _lock_recorder();
    memset(&s_recorder.snapshot, 0, sizeof(s_recorder.snapshot));
    s_recorder.available = true;
    s_recorder.snapshot.generation++;
    s_recorder.snapshot.state = RECORDER_SERVICE_RECORDING;
    s_recorder.snapshot.duration_ms = duration_ms;
    s_recorder.snapshot.storage_ready = true;
    s_recorder.snapshot.storage_generation = 1U;
    s_recorder.snapshot.content_revision = 1U;
    s_recorder.snapshot.index_generation = 1U;
    s_recorder.snapshot.index_revision = 1U;
    s_recorder.snapshot.index_valid = true;
    s_recorder.snapshot.max_duration_ms = 60U * 60U * 1000U;
    _unlock_recorder();
}

void host_recorder_set_available(bool available)
{
    _lock_recorder();
    s_recorder.available = available;
    _unlock_recorder();
}

void host_recorder_set_operation_pending(bool pending)
{
    _lock_recorder();
    s_recorder.snapshot.operation_pending = pending;
    s_recorder.snapshot.generation++;
    _unlock_recorder();
}

void host_recorder_complete_operation(int state, uint32_t duration_ms)
{
    _lock_recorder();
    s_recorder.snapshot.operation_pending = false;
    s_recorder.snapshot.state = (recorder_service_state_t)state;
    s_recorder.snapshot.duration_ms = duration_ms;
    s_recorder.snapshot.generation++;
    _unlock_recorder();
}

void host_recorder_fail_next_command(esp_err_t result)
{
    _lock_recorder();
    s_recorder.fail_next_command = true;
    s_recorder.next_command_result = result;
    _unlock_recorder();
}

unsigned host_recorder_command_count(void)
{
    _lock_recorder();
    const unsigned count = s_recorder.command_count;
    _unlock_recorder();
    return count;
}

unsigned host_recorder_start_count(void)
{
    _lock_recorder();
    const unsigned count = s_recorder.start_count;
    _unlock_recorder();
    return count;
}

unsigned host_recorder_pause_count(void)
{
    _lock_recorder();
    const unsigned count = s_recorder.pause_count;
    _unlock_recorder();
    return count;
}

unsigned host_recorder_resume_count(void)
{
    _lock_recorder();
    const unsigned count = s_recorder.resume_count;
    _unlock_recorder();
    return count;
}

unsigned host_recorder_stop_count(void)
{
    _lock_recorder();
    const unsigned count = s_recorder.stop_count;
    _unlock_recorder();
    return count;
}

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
    _lock_recorder();
    if (!s_recorder.available)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        _unlock_recorder();
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = s_recorder.snapshot;
    _unlock_recorder();
    return ESP_OK;
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

esp_err_t recorder_service_refresh(void)
{
    return ESP_OK;
}

bool recorder_service_is_busy(void)
{
    _lock_recorder();
    const bool busy = s_recorder.snapshot.operation_pending;
    _unlock_recorder();
    return busy;
}

esp_err_t recorder_service_start(void)
{
    return _admit_command(&s_recorder.start_count);
}

esp_err_t recorder_service_pause(void)
{
    return _admit_command(&s_recorder.pause_count);
}

esp_err_t recorder_service_resume(void)
{
    return _admit_command(&s_recorder.resume_count);
}

esp_err_t recorder_service_stop(void)
{
    return _admit_command(&s_recorder.stop_count);
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

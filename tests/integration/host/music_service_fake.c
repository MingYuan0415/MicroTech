#include "music_service.h"

EVENT_BUS_DEFINE_ID(MUSIC_SERVICE_MSG);

#include <string.h>

static music_service_status_t s_status;
static music_player_status_t s_player;

esp_err_t music_service_init(const music_service_config_t *config)
{
    (void)config;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = MUSIC_SERVICE_STATE_READY;
    s_status.logged_in = true;
    s_status.audio_available = true;
    return ESP_OK;
}

esp_err_t music_service_deinit(void)
{
    return ESP_OK;
}
esp_err_t music_service_set_network_ready(bool ready)
{
    (void)ready;
    return ESP_OK;
}
esp_err_t music_service_login_begin(music_login_type_t type)
{
    (void)type;
    return ESP_OK;
}
esp_err_t music_service_login_qr_acquire(const music_image_t **image)
{
    if (image) *image = NULL;
    return ESP_ERR_NOT_FOUND;
}
void music_service_login_qr_release(const music_image_t *image)
{
    (void)image;
}
esp_err_t music_service_login_cancel(void)
{
    return ESP_OK;
}
esp_err_t music_service_logout(void)
{
    return ESP_OK;
}
esp_err_t music_service_reset_persisted_state(void)
{
    return ESP_OK;
}
esp_err_t music_service_refresh_home(bool force)
{
    (void)force;
    return ESP_OK;
}
esp_err_t music_service_home_acquire(const music_home_snapshot_t **snapshot)
{
    if (snapshot) *snapshot = NULL;
    return ESP_ERR_NOT_FOUND;
}
void music_service_home_release(const music_home_snapshot_t *snapshot)
{
    (void)snapshot;
}
esp_err_t music_service_playlist_acquire(const music_playlist_ref_t *ref, uint16_t offset, uint16_t count, const music_playlist_snapshot_t **snapshot)
{
    (void)ref;
    (void)offset;
    (void)count;
    if (snapshot) *snapshot = NULL;
    return ESP_ERR_NOT_FOUND;
}
void music_service_playlist_release(const music_playlist_snapshot_t *snapshot)
{
    (void)snapshot;
}
esp_err_t music_service_lyric_acquire(const char *songmid, const music_lyric_snapshot_t **snapshot)
{
    (void)songmid;
    if (snapshot) *snapshot = NULL;
    return ESP_ERR_NOT_FOUND;
}
void music_service_lyric_release(const music_lyric_snapshot_t *snapshot)
{
    (void)snapshot;
}
esp_err_t music_service_image_acquire(const char *url, uint16_t width, uint16_t height, uint32_t token, const music_image_t **image)
{
    (void)url;
    (void)width;
    (void)height;
    (void)token;
    if (image) *image = NULL;
    return ESP_ERR_NOT_FOUND;
}
void music_service_image_release(const music_image_t *image)
{
    (void)image;
}
esp_err_t music_service_play_playlist(const music_playlist_ref_t *ref, uint16_t start_index)
{
    (void)ref;
    (void)start_index;
    return ESP_OK;
}
esp_err_t music_service_play_pause(void)
{
    return ESP_OK;
}
esp_err_t music_service_next(void)
{
    return ESP_OK;
}
esp_err_t music_service_prev(void)
{
    return ESP_OK;
}
esp_err_t music_service_seek(uint32_t position_ms)
{
    (void)position_ms;
    return ESP_OK;
}
esp_err_t music_service_set_mode(music_play_mode_t mode)
{
    (void)mode;
    return ESP_OK;
}
esp_err_t music_service_stop(void)
{
    return ESP_OK;
}
esp_err_t music_service_get_status(music_service_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    *status = s_status;
    return ESP_OK;
}
esp_err_t music_service_get_player_status(music_player_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    *status = s_player;
    return ESP_OK;
}
bool music_service_is_playing(void)
{
    return false;
}

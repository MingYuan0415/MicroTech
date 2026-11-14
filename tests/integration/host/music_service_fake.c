#include "apps_integration_runtime.h"

#include "music_service.h"
#include "music_snapshot.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define HOST_MUSIC_GENERATION UINT64_C(1)
#define HOST_MUSIC_SONG_COUNT 3U
#define HOST_MUSIC_LYRIC_COUNT 4U
#define HOST_MUSIC_QR_SIZE 21U

EVENT_BUS_DEFINE_ID(MUSIC_SERVICE_MSG);

typedef struct host_music_payloads
{
    music_home_snapshot_t *home;
    music_playlist_snapshot_t *playlist;
    music_queue_snapshot_t *queue;
    music_lyric_snapshot_t *lyric;
    music_image_t *qr;
    music_image_t *image;
} host_music_payloads_t;

typedef struct host_music_state
{
    pthread_mutex_t lock;
    music_service_status_t status;
    music_player_status_t player;
    host_music_payloads_t payloads;
    music_playlist_ref_t play_ref;
    uint64_t generation;
    atomic_uint acquire_count[HOST_MUSIC_RESOURCE_COUNT];
    atomic_uint release_count[HOST_MUSIC_RESOURCE_COUNT];
    atomic_uint active_leases[HOST_MUSIC_RESOURCE_COUNT];
    atomic_uint fail_resource[HOST_MUSIC_RESOURCE_COUNT];
    atomic_int fail_result[HOST_MUSIC_RESOURCE_COUNT];
    atomic_uint delay_count[HOST_MUSIC_RESOURCE_COUNT];
    atomic_uint login_cancel_count;
    bool initialized;
    bool seeded;
    bool status_read_error;
} host_music_state_t;

static host_music_state_t s_music =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void _set_generation_locked(void)
{
    ++s_music.generation;
    if (s_music.generation == 0U)
    {
        ++s_music.generation;
    }
    s_music.status.generation = s_music.generation;
    s_music.player.generation = s_music.generation;
}

static void _release_payloads(host_music_payloads_t *payloads)
{
    music_snapshot_release(payloads->home);
    music_snapshot_release(payloads->playlist);
    music_snapshot_release(payloads->queue);
    music_snapshot_release(payloads->lyric);
    music_snapshot_release(payloads->qr);
    music_snapshot_release(payloads->image);
    memset(payloads, 0, sizeof(*payloads));
}

static void _fill_playlist(music_playlist_t *playlist, uint64_t id,
                           music_playlist_kind_t kind, const char *title,
                           const char *creator, uint32_t track_count)
{
    memset(playlist, 0, sizeof(*playlist));
    playlist->ref.id = id;
    playlist->ref.dirid = kind == MUSIC_PLAYLIST_KIND_MY_CREATED ? 7U :
                          kind == MUSIC_PLAYLIST_KIND_MY_FAVORITE ? 201U : 0U;
    playlist->ref.tid = (uint32_t)id;
    playlist->ref.kind = (uint8_t)kind;
    (void)snprintf(playlist->title, sizeof(playlist->title), "%s", title);
    (void)snprintf(playlist->creator, sizeof(playlist->creator), "%s",
                   creator);
    playlist->track_count = track_count;
    (void)snprintf(playlist->cover_url, sizeof(playlist->cover_url),
                   "https://host.music.local/playlists/%llu.jpg",
                   (unsigned long long)id);
}

static void _fill_song(music_song_t *song, uint16_t index)
{
    memset(song, 0, sizeof(*song));
    song->songid = UINT64_C(700000) + index;
    (void)snprintf(song->songmid, sizeof(song->songmid),
                   "host-song-%u", (unsigned)index + 1U);
    (void)snprintf(song->media_mid, sizeof(song->media_mid),
                   "host-media-%u", (unsigned)index + 1U);
    (void)snprintf(song->title, sizeof(song->title), "测试曲目 %u",
                   (unsigned)index + 1U);
    (void)snprintf(song->artist, sizeof(song->artist), "测试歌手");
    (void)snprintf(song->album, sizeof(song->album), "测试专辑");
    (void)snprintf(song->cover_url, sizeof(song->cover_url),
                   "https://host.music.local/covers/%u.jpg",
                   (unsigned)index + 1U);
    song->duration_ms = 180000U + (uint32_t)index * 5000U;
    song->size_128 = 2400000U;
    song->size_320 = 4800000U;
    song->size_flac = 16000000U;
    song->pay_flags = 1U;
}

static music_home_snapshot_t *_create_home(void)
{
    music_home_snapshot_t *home = music_snapshot_alloc(sizeof(*home), NULL);
    if (home == NULL)
    {
        return NULL;
    }
    home->generation = HOST_MUSIC_GENERATION;
    home->recommended_ok = true;
    home->personal_ok = true;
    home->collected_ok = true;
    home->my_created_count = 1U;
    home->my_favorite_count = 1U;
    home->recommended_count = 2U;
    home->my_collected_count = 1U;
    _fill_playlist(&home->my_created[0], 3001U,
                   MUSIC_PLAYLIST_KIND_MY_CREATED, "我的测试歌单",
                   "测试用户", HOST_MUSIC_SONG_COUNT);
    _fill_playlist(&home->my_favorite[0], 2001U,
                   MUSIC_PLAYLIST_KIND_MY_FAVORITE, "我喜欢的音乐",
                   "测试用户", HOST_MUSIC_SONG_COUNT);
    _fill_playlist(&home->recommended[0], 1001U,
                   MUSIC_PLAYLIST_KIND_RECOMMENDED, "测试推荐",
                   "QQ 音乐", HOST_MUSIC_SONG_COUNT);
    _fill_playlist(&home->recommended[1], 1002U,
                   MUSIC_PLAYLIST_KIND_RECOMMENDED, "测试电台",
                   "QQ 音乐", 8U);
    _fill_playlist(&home->my_collected[0], 4001U,
                   MUSIC_PLAYLIST_KIND_MY_COLLECTED, "测试收藏歌单",
                   "QQ 音乐", HOST_MUSIC_SONG_COUNT);
    return home;
}

static music_playlist_snapshot_t *_create_playlist(void)
{
    const size_t bytes = offsetof(music_playlist_snapshot_t, songs) +
                         HOST_MUSIC_SONG_COUNT * sizeof(music_song_t);
    music_playlist_snapshot_t *playlist =
        music_snapshot_alloc(bytes, NULL);
    if (playlist == NULL)
    {
        return NULL;
    }
    playlist->generation = HOST_MUSIC_GENERATION;
    playlist->total = HOST_MUSIC_SONG_COUNT;
    playlist->count = HOST_MUSIC_SONG_COUNT;
    _fill_playlist(&playlist->playlist, 1001U,
                   MUSIC_PLAYLIST_KIND_RECOMMENDED, "测试推荐",
                   "QQ 音乐", HOST_MUSIC_SONG_COUNT);
    for (uint16_t index = 0U; index < HOST_MUSIC_SONG_COUNT; ++index)
    {
        _fill_song(&playlist->songs[index], index);
    }
    return playlist;
}

static music_queue_snapshot_t *_create_queue(void)
{
    const size_t bytes = offsetof(music_queue_snapshot_t, songs) +
                         HOST_MUSIC_SONG_COUNT * sizeof(music_song_t);
    music_queue_snapshot_t *queue = music_snapshot_alloc(bytes, NULL);
    if (queue == NULL)
    {
        return NULL;
    }
    queue->generation = HOST_MUSIC_GENERATION;
    queue->ref.id = 1001U;
    queue->ref.kind = MUSIC_PLAYLIST_KIND_RECOMMENDED;
    queue->total = HOST_MUSIC_SONG_COUNT;
    queue->offset = 0U;
    queue->count = HOST_MUSIC_SONG_COUNT;
    for (uint16_t index = 0U; index < HOST_MUSIC_SONG_COUNT; ++index)
    {
        _fill_song(&queue->songs[index], index);
    }
    return queue;
}

static music_lyric_snapshot_t *_create_lyric(void)
{
    const size_t bytes = offsetof(music_lyric_snapshot_t, lines) +
                         HOST_MUSIC_LYRIC_COUNT * sizeof(music_lyric_line_t);
    music_lyric_snapshot_t *lyric = music_snapshot_alloc(bytes, NULL);
    if (lyric == NULL)
    {
        return NULL;
    }
    lyric->generation = HOST_MUSIC_GENERATION;
    lyric->line_count = HOST_MUSIC_LYRIC_COUNT;
    static const uint32_t offsets[HOST_MUSIC_LYRIC_COUNT] =
    {
        0U, 15000U, 30000U, 45000U,
    };
    static const char *const lines[HOST_MUSIC_LYRIC_COUNT] =
    {
        "第一行测试歌词",
        "第二行测试歌词",
        "第三行测试歌词",
        "第四行测试歌词",
    };
    for (size_t index = 0U; index < HOST_MUSIC_LYRIC_COUNT; ++index)
    {
        lyric->lines[index].offset_ms = offsets[index];
        (void)snprintf(lyric->lines[index].text,
                       sizeof(lyric->lines[index].text), "%s", lines[index]);
    }
    return lyric;
}

static music_image_t *_create_image(uint16_t width, uint16_t height,
                                    uint32_t token, const char *url)
{
    const size_t pixel_bytes = (size_t)width * height * sizeof(uint16_t);
    music_image_t *image = music_snapshot_alloc(
                               sizeof(*image) + pixel_bytes, NULL);
    if (image == NULL)
    {
        return NULL;
    }
    image->width = width;
    image->height = height;
    image->requested_width = width;
    image->requested_height = height;
    image->data_size = pixel_bytes;
    image->pixels = (uint8_t *)(image + 1);
    image->token = token;
    (void)snprintf(image->url, sizeof(image->url), "%s", url);
    memset(image->pixels, 0x5A, pixel_bytes);
    return image;
}

static void _set_player_from_playlist(void)
{
    const music_song_t *song = &s_music.payloads.playlist->songs[0];
    s_music.player.state = MUSIC_PLAYBACK_PLAYING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.mode = MUSIC_PLAY_MODE_REPEAT_ALL;
    s_music.player.quality = MUSIC_QUALITY_HIGH;
    s_music.player.queue_index = 0U;
    s_music.player.queue_count = HOST_MUSIC_SONG_COUNT;
    s_music.player.position_ms = 42000U;
    s_music.player.duration_ms = song->duration_ms;
    s_music.player.buffered_ms = 0U;
    s_music.player.play_generation = HOST_MUSIC_GENERATION;
    (void)snprintf(s_music.player.title, sizeof(s_music.player.title), "%s",
                   song->title);
    (void)snprintf(s_music.player.artist, sizeof(s_music.player.artist), "%s",
                   song->artist);
    (void)snprintf(s_music.player.cover_url,
                   sizeof(s_music.player.cover_url), "%s", song->cover_url);
    (void)snprintf(s_music.player.songmid, sizeof(s_music.player.songmid),
                   "%s", song->songmid);
    s_music.play_ref = s_music.payloads.playlist->playlist.ref;
}

static esp_err_t _create_seed_payloads(host_music_payloads_t *payloads)
{
    memset(payloads, 0, sizeof(*payloads));
    payloads->home = _create_home();
    payloads->playlist = _create_playlist();
    payloads->queue = _create_queue();
    payloads->lyric = _create_lyric();
    if (payloads->home == NULL || payloads->playlist == NULL ||
            payloads->queue == NULL || payloads->lyric == NULL)
    {
        _release_payloads(payloads);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void _publish(uint32_t sub_type, uint32_t token,
                     music_service_failure_t failure)
{
    music_service_event_t event;
    memset(&event, 0, sizeof(event));
    (void)pthread_mutex_lock(&s_music.lock);
    event.generation = s_music.generation;
    event.sub_type = sub_type;
    event.token = token;
    event.state = s_music.status.state;
    event.failure = failure;
    (void)pthread_mutex_unlock(&s_music.lock);
    (void)event_bus_publish(MUSIC_SERVICE_MSG, sub_type, &event,
                            sizeof(event), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

static esp_err_t _check_acquire(int resource, esp_err_t *result)
{
    assert(resource >= 0 && resource < HOST_MUSIC_RESOURCE_COUNT);
    assert(result != NULL);
    const unsigned delay = atomic_load(&s_music.delay_count[resource]);
    if (delay > 0U)
    {
        atomic_fetch_sub(&s_music.delay_count[resource], 1U);
        *result = ESP_ERR_NOT_FOUND;
        return ESP_FAIL;
    }
    const unsigned failures = atomic_load(&s_music.fail_resource[resource]);
    if (failures > 0U)
    {
        atomic_fetch_sub(&s_music.fail_resource[resource], 1U);
        *result = (esp_err_t)atomic_load(&s_music.fail_result[resource]);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const void *_acquire_payload(int resource, const void *payload)
{
    const void *leased = music_snapshot_retain((void *)payload);
    if (leased != NULL)
    {
        atomic_fetch_add(&s_music.acquire_count[resource], 1U);
        atomic_fetch_add(&s_music.active_leases[resource], 1U);
    }
    return leased;
}

static void _release_payload(int resource, const void *payload)
{
    if (payload == NULL)
    {
        return;
    }
    music_snapshot_release((void *)payload);
    atomic_fetch_add(&s_music.release_count[resource], 1U);
    atomic_fetch_sub(&s_music.active_leases[resource], 1U);
}

void host_music_reset(void)
{
    host_music_payloads_t old;
    memset(&old, 0, sizeof(old));
    (void)pthread_mutex_lock(&s_music.lock);
    old = s_music.payloads;
    memset(&s_music.payloads, 0, sizeof(s_music.payloads));
    memset(&s_music.status, 0, sizeof(s_music.status));
    memset(&s_music.player, 0, sizeof(s_music.player));
    memset(&s_music.play_ref, 0, sizeof(s_music.play_ref));
    s_music.generation = HOST_MUSIC_GENERATION;
    s_music.status.generation = HOST_MUSIC_GENERATION;
    s_music.status.state = MUSIC_SERVICE_STATE_READY;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.network_ready = true;
    s_music.status.time_ready = true;
    s_music.status.logged_in = true;
    s_music.status.audio_available = true;
    s_music.status.login_type = MUSIC_LOGIN_TYPE_QQ;
    (void)snprintf(s_music.status.user_id, sizeof(s_music.status.user_id),
                   "%s", "123456789");
    (void)snprintf(s_music.status.user_name,
                   sizeof(s_music.status.user_name), "测试用户");
    (void)snprintf(s_music.status.user_avatar,
                   sizeof(s_music.status.user_avatar), "%s",
                   "https://q.qlogo.cn/headimg_dl?dst_uin=123456789&spec=140");
    s_music.player.generation = HOST_MUSIC_GENERATION;
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    s_music.player.quality = MUSIC_QUALITY_HIGH;
    s_music.seeded = false;
    s_music.initialized = false;
    s_music.status_read_error = false;
    for (int resource = 0; resource < HOST_MUSIC_RESOURCE_COUNT; ++resource)
    {
        atomic_store(&s_music.acquire_count[resource], 0U);
        atomic_store(&s_music.release_count[resource], 0U);
        atomic_store(&s_music.active_leases[resource], 0U);
        atomic_store(&s_music.fail_resource[resource], 0U);
        atomic_store(&s_music.fail_result[resource], ESP_OK);
        atomic_store(&s_music.delay_count[resource], 0U);
    }
    atomic_store(&s_music.login_cancel_count, 0U);
    (void)pthread_mutex_unlock(&s_music.lock);
    _release_payloads(&old);
}

void host_music_seed_success(void)
{
    host_music_payloads_t payloads;
    host_music_payloads_t old;
    memset(&old, 0, sizeof(old));
    const esp_err_t result = _create_seed_payloads(&payloads);
    assert(result == ESP_OK);
    (void)pthread_mutex_lock(&s_music.lock);
    old = s_music.payloads;
    s_music.payloads = payloads;
    s_music.seeded = true;
    _set_player_from_playlist();
    (void)pthread_mutex_unlock(&s_music.lock);
    _release_payloads(&old);
}

void host_music_fail_next_acquire(int resource, esp_err_t result)
{
    assert(resource >= 0 && resource < HOST_MUSIC_RESOURCE_COUNT);
    atomic_store(&s_music.fail_result[resource], result);
    atomic_fetch_add(&s_music.fail_resource[resource], 1U);
}

void host_music_delay_acquire(int resource, unsigned attempts)
{
    assert(resource >= 0 && resource < HOST_MUSIC_RESOURCE_COUNT);
    atomic_store(&s_music.delay_count[resource], attempts);
}

unsigned host_music_active_lease_count(int resource)
{
    assert(resource >= 0 && resource < HOST_MUSIC_RESOURCE_COUNT);
    return atomic_load(&s_music.active_leases[resource]);
}

unsigned host_music_acquire_count(int resource)
{
    assert(resource >= 0 && resource < HOST_MUSIC_RESOURCE_COUNT);
    return atomic_load(&s_music.acquire_count[resource]);
}

unsigned host_music_release_count(int resource)
{
    assert(resource >= 0 && resource < HOST_MUSIC_RESOURCE_COUNT);
    return atomic_load(&s_music.release_count[resource]);
}

unsigned host_music_login_cancel_count(void)
{
    return atomic_load(&s_music.login_cancel_count);
}

void host_music_set_player_state(int state, int failure)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.player.state = (music_playback_state_t)state;
    s_music.player.failure = (music_service_failure_t)failure;
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
}

void host_music_set_status_read_error(bool enabled)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.status_read_error = enabled;
    (void)pthread_mutex_unlock(&s_music.lock);
}

esp_err_t music_service_init(const music_service_config_t *config)
{
    (void)config;
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.initialized = true;
    (void)pthread_mutex_unlock(&s_music.lock);
    return ESP_OK;
}

esp_err_t music_service_deinit(void)
{
    host_music_reset();
    return ESP_OK;
}

esp_err_t music_service_set_network_ready(bool ready)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.status.network_ready = ready;
    if (!ready)
    {
        s_music.status.state = MUSIC_SERVICE_STATE_WAITING_NETWORK;
        s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_set_time_ready(bool ready)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.status.time_ready = ready;
    if (!ready && s_music.status.state != MUSIC_SERVICE_STATE_WAITING_NETWORK)
    {
        s_music.status.state = MUSIC_SERVICE_STATE_WAITING_TIME;
        s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_retry(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    if (s_music.status.state == MUSIC_SERVICE_STATE_ERROR)
    {
        s_music.status.state = s_music.status.logged_in ?
                               MUSIC_SERVICE_STATE_READY :
                               MUSIC_SERVICE_STATE_LOGGED_OUT;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U,
             s_music.status.failure);
    return ESP_OK;
}

esp_err_t music_service_login_begin(music_login_type_t type)
{
    if (type != MUSIC_LOGIN_TYPE_QQ && type != MUSIC_LOGIN_TYPE_WECHAT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    music_image_t *qr = _create_image(HOST_MUSIC_QR_SIZE, HOST_MUSIC_QR_SIZE,
                                      (uint32_t)type + 1U,
                                      "https://host.music.local/login/qr");
    if (qr == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    music_image_t *old = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    if (!s_music.status.network_ready || !s_music.status.time_ready)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        music_snapshot_release(qr);
        return ESP_ERR_INVALID_STATE;
    }
    old = s_music.payloads.qr;
    s_music.payloads.qr = qr;
    s_music.status.state = MUSIC_SERVICE_STATE_QR_READY;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.qr_available = true;
    s_music.status.login_type = type;
    (void)snprintf(s_music.status.qr_status,
                   sizeof(s_music.status.qr_status), "%s",
                   type == MUSIC_LOGIN_TYPE_QQ ? "请使用QQ扫码" :
                   "请使用微信扫码");
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    music_snapshot_release(old);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_login_qr_acquire(const music_image_t **image)
{
    if (image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_QR, &result) != ESP_OK)
    {
        return result;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    const music_image_t *leased =
        _acquire_payload(HOST_MUSIC_RESOURCE_QR, s_music.payloads.qr);
    (void)pthread_mutex_unlock(&s_music.lock);
    if (leased == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *image = leased;
    return ESP_OK;
}

void music_service_login_qr_release(const music_image_t *image)
{
    _release_payload(HOST_MUSIC_RESOURCE_QR, image);
}

esp_err_t music_service_login_cancel(void)
{
    music_image_t *old = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    old = s_music.payloads.qr;
    s_music.payloads.qr = NULL;
    if (s_music.status.state == MUSIC_SERVICE_STATE_QR_READY ||
            s_music.status.state == MUSIC_SERVICE_STATE_QR_SCANNED ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTHENTICATING)
    {
        s_music.status.state = s_music.status.logged_in ?
                               MUSIC_SERVICE_STATE_READY :
                               MUSIC_SERVICE_STATE_LOGGED_OUT;
        s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    }
    s_music.status.qr_available = false;
    s_music.status.qr_status[0] = '\0';
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    music_snapshot_release(old);
    atomic_fetch_add(&s_music.login_cancel_count, 1U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_logout(void)
{
    host_music_payloads_t old;
    memset(&old, 0, sizeof(old));
    (void)pthread_mutex_lock(&s_music.lock);
    old = s_music.payloads;
    memset(&s_music.payloads, 0, sizeof(s_music.payloads));
    s_music.status.logged_in = false;
    s_music.status.qr_available = false;
    s_music.status.state = MUSIC_SERVICE_STATE_LOGGED_OUT;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.user_id[0] = '\0';
    s_music.status.user_name[0] = '\0';
    s_music.status.user_avatar[0] = '\0';
    s_music.status.qr_status[0] = '\0';
    memset(&s_music.player, 0, sizeof(s_music.player));
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    s_music.player.quality = MUSIC_QUALITY_HIGH;
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _release_payloads(&old);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_reset_persisted_state(void)
{
    return ESP_OK;
}

esp_err_t music_service_refresh_home(bool force)
{
    (void)force;
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_home_acquire(const music_home_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_HOME, &result) != ESP_OK)
    {
        return result;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    const music_home_snapshot_t *leased =
        _acquire_payload(HOST_MUSIC_RESOURCE_HOME, s_music.payloads.home);
    (void)pthread_mutex_unlock(&s_music.lock);
    if (leased == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = leased;
    return ESP_OK;
}

void music_service_home_release(const music_home_snapshot_t *snapshot)
{
    _release_payload(HOST_MUSIC_RESOURCE_HOME, snapshot);
}

esp_err_t music_service_playlist_acquire(const music_playlist_ref_t *ref,
        uint16_t offset, uint16_t count,
        const music_playlist_snapshot_t **snapshot)
{
    if (snapshot == NULL || count == 0U || count > MUSIC_SERVICE_MAX_SONGS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_PLAYLIST, &result) != ESP_OK)
    {
        return result;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    const music_playlist_snapshot_t *payload = s_music.payloads.playlist;
    const bool direct = ref == NULL || ref->id == 0U;
    const bool matches = payload != NULL && offset == payload->offset &&
                         (direct ||
                          (payload->playlist.ref.id == ref->id &&
                           payload->playlist.ref.dirid == ref->dirid &&
                           payload->playlist.ref.tid == ref->tid &&
                           payload->playlist.ref.kind == ref->kind));
    const music_playlist_snapshot_t *leased =
        matches ? _acquire_payload(HOST_MUSIC_RESOURCE_PLAYLIST, payload) :
        NULL;
    (void)pthread_mutex_unlock(&s_music.lock);
    if (leased == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = leased;
    return ESP_OK;
}

void music_service_playlist_release(const music_playlist_snapshot_t *snapshot)
{
    _release_payload(HOST_MUSIC_RESOURCE_PLAYLIST, snapshot);
}

esp_err_t music_service_queue_acquire(const music_queue_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_QUEUE, &result) != ESP_OK)
    {
        return result;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    const music_queue_snapshot_t *leased =
        _acquire_payload(HOST_MUSIC_RESOURCE_QUEUE, s_music.payloads.queue);
    (void)pthread_mutex_unlock(&s_music.lock);
    if (leased == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = leased;
    return ESP_OK;
}

esp_err_t music_service_queue_view_acquire(uint16_t offset, uint16_t count,
        const music_queue_snapshot_t **snapshot)
{
    if (snapshot == NULL || count == 0U || count > MUSIC_SERVICE_MAX_SONGS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_QUEUE, &result) != ESP_OK)
    {
        return result;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    const music_queue_snapshot_t *payload = s_music.payloads.queue;
    const bool matches = payload != NULL && offset == payload->offset;
    const music_queue_snapshot_t *leased = matches ?
                                           _acquire_payload(
                                                   HOST_MUSIC_RESOURCE_QUEUE,
                                                   payload) : NULL;
    (void)pthread_mutex_unlock(&s_music.lock);
    if (leased == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = leased;
    return ESP_OK;
}

void music_service_queue_release(const music_queue_snapshot_t *snapshot)
{
    _release_payload(HOST_MUSIC_RESOURCE_QUEUE, snapshot);
}

esp_err_t music_service_lyric_acquire(const char *songmid,
                                      const music_lyric_snapshot_t **snapshot)
{
    if (songmid == NULL || songmid[0] == '\0' || snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_LYRIC, &result) != ESP_OK)
    {
        return result;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    const bool matches = s_music.payloads.lyric != NULL &&
                         s_music.player.songmid[0] != '\0' &&
                         strcmp(s_music.player.songmid, songmid) == 0;
    const music_lyric_snapshot_t *leased =
        matches ? _acquire_payload(HOST_MUSIC_RESOURCE_LYRIC,
                                   s_music.payloads.lyric) : NULL;
    (void)pthread_mutex_unlock(&s_music.lock);
    if (leased == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = leased;
    return ESP_OK;
}

void music_service_lyric_release(const music_lyric_snapshot_t *snapshot)
{
    _release_payload(HOST_MUSIC_RESOURCE_LYRIC, snapshot);
}

bool music_service_lyric_failed(const char *songmid)
{
    (void)songmid;
    return false;
}

void music_service_lyric_retry(const char *songmid)
{
    (void)songmid;
}

esp_err_t music_service_image_acquire(const char *url, uint16_t width,
                                      uint16_t height, uint32_t token,
                                      const music_image_t **image)
{
    if (url == NULL || url[0] == '\0' || width == 0U || height == 0U ||
            image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    esp_err_t result = ESP_OK;
    if (_check_acquire(HOST_MUSIC_RESOURCE_IMAGE, &result) != ESP_OK)
    {
        return result;
    }
    music_image_t *old = NULL;
    music_image_t *created = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.payloads.image != NULL &&
            s_music.payloads.image->token == token &&
            s_music.payloads.image->requested_width == width &&
            s_music.payloads.image->requested_height == height &&
            strcmp(s_music.payloads.image->url, url) == 0)
    {
        created = music_snapshot_retain(s_music.payloads.image);
        if (created != NULL)
        {
            atomic_fetch_add(
                &s_music.acquire_count[HOST_MUSIC_RESOURCE_IMAGE], 1U);
            atomic_fetch_add(
                &s_music.active_leases[HOST_MUSIC_RESOURCE_IMAGE], 1U);
        }
        (void)pthread_mutex_unlock(&s_music.lock);
        *image = created;
        return created != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    created = _create_image(width, height, token, url);
    if (created == NULL)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NO_MEM;
    }
    old = s_music.payloads.image;
    s_music.payloads.image = created;
    (void)pthread_mutex_unlock(&s_music.lock);
    music_snapshot_release(old);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE, token,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_ERR_NOT_FOUND;
}

void music_service_image_release(const music_image_t *image)
{
    _release_payload(HOST_MUSIC_RESOURCE_IMAGE, image);
}

esp_err_t music_service_play_playlist(const music_playlist_ref_t *ref,
                                      uint16_t start_index)
{
    if (ref == NULL || start_index >= HOST_MUSIC_SONG_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.payloads.playlist == NULL ||
            s_music.payloads.playlist->playlist.ref.id != ref->id)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NOT_FOUND;
    }
    _set_player_from_playlist();
    const music_song_t *song = &s_music.payloads.playlist->songs[start_index];
    (void)snprintf(s_music.player.title, sizeof(s_music.player.title), "%s",
                   song->title);
    (void)snprintf(s_music.player.songmid, sizeof(s_music.player.songmid),
                   "%s", song->songmid);
    s_music.player.queue_index = start_index;
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_play_playlist_snapshot(
    const music_playlist_snapshot_t *playlist, uint16_t start_index)
{
    if (playlist == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return music_service_play_playlist(&playlist->playlist.ref, start_index);
}

esp_err_t music_service_queue_play(uint16_t index)
{
    (void)pthread_mutex_lock(&s_music.lock);
    music_queue_snapshot_t *queue = s_music.payloads.queue;
    if (queue == NULL || index >= queue->count)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_ARG;
    }
    const music_song_t *song = &queue->songs[index];
    s_music.player.state = MUSIC_PLAYBACK_PLAYING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.queue_index = index;
    s_music.player.queue_count = queue->count;
    s_music.player.duration_ms = song->duration_ms;
    s_music.player.position_ms = 0U;
    (void)snprintf(s_music.player.title, sizeof(s_music.player.title), "%s",
                   song->title);
    (void)snprintf(s_music.player.artist, sizeof(s_music.player.artist), "%s",
                   song->artist);
    (void)snprintf(s_music.player.songmid, sizeof(s_music.player.songmid),
                   "%s", song->songmid);
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_queue_remove(uint16_t index)
{
    (void)pthread_mutex_lock(&s_music.lock);
    music_queue_snapshot_t *queue = s_music.payloads.queue;
    if (queue == NULL || index >= queue->count)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t old_count = queue->count;
    music_queue_snapshot_t *next = NULL;
    if (old_count > 1U)
    {
        const size_t bytes = offsetof(music_queue_snapshot_t, songs) +
                             (size_t)(old_count - 1U) * sizeof(music_song_t);
        next = music_snapshot_alloc(bytes, NULL);
        if (next == NULL)
        {
            (void)pthread_mutex_unlock(&s_music.lock);
            return ESP_ERR_NO_MEM;
        }
        next->generation = HOST_MUSIC_GENERATION + 1U;
        next->count = (uint16_t)(old_count - 1U);
        if (index > 0U)
        {
            memcpy(next->songs, queue->songs,
                   (size_t)index * sizeof(music_song_t));
        }
        if (index + 1U < old_count)
        {
            memcpy(&next->songs[index], &queue->songs[index + 1U],
                   (size_t)(old_count - index - 1U) *
                   sizeof(music_song_t));
        }
    }
    music_queue_snapshot_t *old = queue;
    s_music.payloads.queue = next;
    if (next == NULL)
    {
        memset(&s_music.player, 0, sizeof(s_music.player));
        s_music.player.state = MUSIC_PLAYBACK_IDLE;
        s_music.player.queue_index = 0U;
        s_music.player.queue_count = 0U;
    }
    else
    {
        if (s_music.player.queue_index > index)
        {
            --s_music.player.queue_index;
        }
        else if (s_music.player.queue_index == index &&
                 s_music.player.queue_index >= next->count)
        {
            s_music.player.queue_index = (uint16_t)(next->count - 1U);
        }
        s_music.player.queue_count = next->count;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    music_snapshot_release(old);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_play_pause(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.player.state == MUSIC_PLAYBACK_PLAYING)
    {
        s_music.player.state = MUSIC_PLAYBACK_PAUSED;
    }
    else if (s_music.player.state == MUSIC_PLAYBACK_PAUSED)
    {
        s_music.player.state = MUSIC_PLAYBACK_PLAYING;
    }
    else
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

static esp_err_t _step(bool forward)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.payloads.playlist == NULL ||
            s_music.player.state == MUSIC_PLAYBACK_IDLE ||
            s_music.player.state == MUSIC_PLAYBACK_ERROR)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t count = s_music.payloads.playlist->count;
    uint16_t index = s_music.player.queue_index;
    if (forward && index + 1U < count)
    {
        ++index;
    }
    else if (!forward && index > 0U)
    {
        --index;
    }
    else
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const music_song_t *song = &s_music.payloads.playlist->songs[index];
    s_music.player.queue_index = index;
    (void)snprintf(s_music.player.title, sizeof(s_music.player.title), "%s",
                   song->title);
    (void)snprintf(s_music.player.songmid, sizeof(s_music.player.songmid),
                   "%s", song->songmid);
    s_music.player.duration_ms = song->duration_ms;
    s_music.player.position_ms = 0U;
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_next(void)
{
    return _step(true);
}

esp_err_t music_service_prev(void)
{
    return _step(false);
}

esp_err_t music_service_seek(uint32_t position_ms)
{
    (void)position_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t music_service_set_mode(music_play_mode_t mode)
{
    if ((int)mode < (int)MUSIC_PLAY_MODE_SEQUENTIAL ||
            mode > MUSIC_PLAY_MODE_SHUFFLE)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.player.mode = mode;
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_stop(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    memset(&s_music.player, 0, sizeof(s_music.player));
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    s_music.player.quality = MUSIC_QUALITY_HIGH;
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U,
             MUSIC_SERVICE_FAILURE_NONE);
    return ESP_OK;
}

esp_err_t music_service_get_status(music_service_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    *status = s_music.status;
    (void)pthread_mutex_unlock(&s_music.lock);
    return ESP_OK;
}

esp_err_t music_service_get_player_status(music_player_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.status_read_error)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    *status = s_music.player;
    (void)pthread_mutex_unlock(&s_music.lock);
    return ESP_OK;
}

esp_err_t music_service_get_diagnostics(
    music_service_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    return ESP_OK;
}

bool music_service_is_playing(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    const bool playing = s_music.player.state == MUSIC_PLAYBACK_PLAYING;
    (void)pthread_mutex_unlock(&s_music.lock);
    return playing;
}

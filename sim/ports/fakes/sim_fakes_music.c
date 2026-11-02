#include "sim_fakes_music.h"

#include "music_service.h"
#include "music_service_cache.h"
#include "music_snapshot.h"

#include "event_bus.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SIM_MUSIC_FIXTURE_GENERATION UINT64_C(1)
#define SIM_MUSIC_QR_SIZE 49U
#define SIM_MUSIC_CATALOG_COUNT 5U
#define SIM_MUSIC_PARTIAL_COUNT 2U
#define SIM_MUSIC_PARTIAL_TOTAL 12U
#define SIM_MUSIC_LONG_COUNT 10U
#define SIM_MUSIC_LONG_TOTAL 137U

EVENT_BUS_DEFINE_ID(MUSIC_SERVICE_MSG);

typedef struct sim_music_state
{
    pthread_mutex_t lock;
    const char *fixture;
    uint64_t generation;
    music_service_status_t status;
    music_player_status_t player;
    music_home_snapshot_t *home;
    music_playlist_snapshot_t *playlist;
    music_lyric_snapshot_t *lyric;
    music_image_t *login_qr;
    music_image_t *image;
    music_playlist_ref_t play_ref;
    music_song_t queue[MUSIC_SERVICE_MAX_SONGS];
    uint16_t queue_count;
    uint16_t queue_index;
    bool queue_valid;
    bool play_valid;
    bool player_unavailable;
    bool playlist_failed;
    uint8_t playlist_failures_remaining;
    bool queue_failed;
    bool lyric_failed;
    char lyric_songmid[MUSIC_SERVICE_SONGMID_BYTES];
} sim_music_state_t;

typedef struct sim_music_fixture
{
    const char *name;
    music_service_state_t state;
    music_service_failure_t failure;
    music_player_status_t player;
    music_home_snapshot_t *home;
    music_playlist_snapshot_t *playlist;
    music_lyric_snapshot_t *lyric;
    music_image_t *login_qr;
    music_playlist_ref_t play_ref;
    music_song_t queue[MUSIC_SERVICE_MAX_SONGS];
    uint16_t queue_count;
    uint16_t queue_index;
    bool queue_valid;
    bool play_valid;
    bool player_unavailable;
    bool playlist_failed;
    uint8_t playlist_failures_remaining;
    bool queue_failed;
    bool lyric_failed;
    char lyric_songmid[MUSIC_SERVICE_SONGMID_BYTES];
} sim_music_fixture_t;

static sim_music_state_t s_music =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fixture = "empty",
    .generation = SIM_MUSIC_FIXTURE_GENERATION,
    .status =
    {
        .generation = SIM_MUSIC_FIXTURE_GENERATION,
        .state = MUSIC_SERVICE_STATE_LOGGED_OUT,
        .failure = MUSIC_SERVICE_FAILURE_NONE,
        .network_ready = true,
        .time_ready = true,
        .logged_in = false,
        .audio_available = true,
        .login_type = MUSIC_LOGIN_TYPE_QQ,
        .user_name = "",
    },
    .player =
    {
        .generation = SIM_MUSIC_FIXTURE_GENERATION,
        .state = MUSIC_PLAYBACK_IDLE,
        .failure = MUSIC_SERVICE_FAILURE_NONE,
        .mode = MUSIC_PLAY_MODE_SEQUENTIAL,
        .quality = MUSIC_QUALITY_HIGH,
    },
};

static void _release_fixture(sim_music_fixture_t *fixture)
{
    if (fixture == NULL)
    {
        return;
    }
    music_snapshot_release(fixture->home);
    music_snapshot_release(fixture->playlist);
    music_snapshot_release(fixture->lyric);
    music_snapshot_release(fixture->login_qr);
    memset(fixture, 0, sizeof(*fixture));
}

static void _release_state_locked(sim_music_state_t *state,
                                  music_home_snapshot_t **home,
                                  music_playlist_snapshot_t **playlist,
                                  music_lyric_snapshot_t **lyric,
                                  music_image_t **login_qr,
                                  music_image_t **image)
{
    *home = state->home;
    *playlist = state->playlist;
    *lyric = state->lyric;
    *login_qr = state->login_qr;
    *image = state->image;
    state->home = NULL;
    state->playlist = NULL;
    state->lyric = NULL;
    state->login_qr = NULL;
    state->image = NULL;
}

static void _release_detached(music_home_snapshot_t *home,
                              music_playlist_snapshot_t *playlist,
                              music_lyric_snapshot_t *lyric,
                              music_image_t *login_qr,
                              music_image_t *image)
{
    music_snapshot_release(home);
    music_snapshot_release(playlist);
    music_snapshot_release(lyric);
    music_snapshot_release(login_qr);
    music_snapshot_release(image);
}

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

static void _publish(uint32_t sub_type, uint32_t token)
{
    music_service_event_t event;
    memset(&event, 0, sizeof(event));
    (void)pthread_mutex_lock(&s_music.lock);
    event.generation = s_music.generation;
    event.sub_type = sub_type;
    event.token = token;
    event.state = s_music.status.state;
    event.failure = s_music.status.failure;
    (void)pthread_mutex_unlock(&s_music.lock);
    (void)event_bus_publish(MUSIC_SERVICE_MSG, sub_type, &event,
                            sizeof(event), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

static void _publish_fixture(void)
{
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U);
}

static void _fill_playlist(music_playlist_t *playlist, uint64_t id,
                           uint32_t dirid, uint32_t tid,
                           music_playlist_kind_t kind, const char *title,
                           const char *creator, uint32_t track_count)
{
    memset(playlist, 0, sizeof(*playlist));
    playlist->ref.id = id;
    playlist->ref.dirid = dirid;
    playlist->ref.tid = tid;
    playlist->ref.kind = (uint8_t)kind;
    (void)snprintf(playlist->title, sizeof(playlist->title), "%s", title);
    (void)snprintf(playlist->creator, sizeof(playlist->creator), "%s",
                   creator);
    playlist->track_count = track_count;
    (void)snprintf(playlist->cover_url, sizeof(playlist->cover_url),
                   "https://sim.music.local/covers/playlist/%llu.jpg",
                   (unsigned long long)id);
}

static music_home_snapshot_t *_create_home(uint32_t catalog_total)
{
    music_home_snapshot_t *home =
        music_snapshot_alloc(sizeof(*home), NULL);
    if (home == NULL)
    {
        return NULL;
    }
    home->generation = SIM_MUSIC_FIXTURE_GENERATION;
    home->recommended_ok = true;
    home->personal_ok = true;
    home->collected_ok = true;
    home->recommended_count = 3U;
    home->my_favorite_count = 2U;
    home->my_created_count = 1U;
    home->my_collected_count = 2U;
    _fill_playlist(&home->recommended[0], 1001U, 0U, 1001U,
                   MUSIC_PLAYLIST_KIND_RECOMMENDED, "模拟晨间歌单",
                   "模拟音乐", catalog_total);
    _fill_playlist(&home->recommended[1], 1002U, 0U, 1002U,
                   MUSIC_PLAYLIST_KIND_RECOMMENDED, "专注节拍",
                   "模拟音乐", 18U);
    _fill_playlist(&home->recommended[2], 1003U, 0U, 1003U,
                   MUSIC_PLAYLIST_KIND_RECOMMENDED, "夜间电台",
                   "模拟音乐", 24U);
    _fill_playlist(&home->my_favorite[0], 0U, 201U, 0U,
                   MUSIC_PLAYLIST_KIND_MY_FAVORITE, "我喜欢的音乐",
                   "模拟用户", catalog_total);
    _fill_playlist(&home->my_favorite[1], 2002U, 0U, 2002U,
                   MUSIC_PLAYLIST_KIND_MY_FAVORITE, "循环清单",
                   "模拟用户", 9U);
    _fill_playlist(&home->my_created[0], 3001U, 7U, 3001U,
                   MUSIC_PLAYLIST_KIND_MY_CREATED, "我的现场",
                   "模拟用户", 11U);
    _fill_playlist(&home->my_collected[0], 4001U, 0U, 4001U,
                   MUSIC_PLAYLIST_KIND_MY_COLLECTED, "收藏节拍",
                   "模拟音乐", 16U);
    _fill_playlist(&home->my_collected[1], 4002U, 0U, 4002U,
                   MUSIC_PLAYLIST_KIND_MY_COLLECTED, "深夜收藏",
                   "模拟音乐", 8U);
    return home;
}

static void _fill_song(music_song_t *song, uint16_t index)
{
    memset(song, 0, sizeof(*song));
    song->songid = UINT64_C(900000) + index;
    (void)snprintf(song->songmid, sizeof(song->songmid),
                   "sim-song-%03u", (unsigned)index + 1U);
    (void)snprintf(song->media_mid, sizeof(song->media_mid),
                   "sim-media-%03u", (unsigned)index + 1U);
    (void)snprintf(song->title, sizeof(song->title), "模拟曲目 %02u",
                   (unsigned)index + 1U);
    (void)snprintf(song->artist, sizeof(song->artist), "确定性乐队");
    (void)snprintf(song->album, sizeof(song->album), "模拟现场");
    song->duration_ms = 180000U + (uint32_t)index * 7000U;
    song->size_128 = 2400000U + (uint32_t)index * 4096U;
    song->size_320 = 4800000U + (uint32_t)index * 8192U;
    song->size_flac = 16000000U + (uint32_t)index * 16384U;
    song->pay_flags = 1U;
    (void)snprintf(song->cover_url, sizeof(song->cover_url),
                   "https://sim.music.local/covers/song/%03u.jpg",
                   (unsigned)index + 1U);
}

static music_playlist_snapshot_t *_create_playlist(uint16_t count,
        uint32_t total)
{
    const size_t bytes = offsetof(music_playlist_snapshot_t, songs) +
                         (size_t)count * sizeof(music_song_t);
    music_playlist_snapshot_t *playlist =
        music_snapshot_alloc(bytes, NULL);
    if (playlist == NULL)
    {
        return NULL;
    }
    playlist->generation = SIM_MUSIC_FIXTURE_GENERATION;
    playlist->total = total;
    playlist->offset = 0U;
    playlist->count = count;
    _fill_playlist(&playlist->playlist, 0U, 201U, 0U,
                   MUSIC_PLAYLIST_KIND_MY_FAVORITE, "我喜欢的音乐",
                   "模拟用户", total);
    for (uint16_t index = 0U; index < count; ++index)
    {
        _fill_song(&playlist->songs[index], index);
    }
    return playlist;
}

static music_lyric_snapshot_t *_create_lyric(void)
{
    static const uint32_t offsets[] =
    {
        0U, 15000U, 30000U, 45000U, 60000U, 75000U, 90000U,
    };
    static const char *const texts[] =
    {
        "第一束模拟晨光",
        "落在安静的屏幕",
        "节拍沿着时间向前",
        "我们把此刻留在歌里",
        "下一段旋律正在靠近",
        "确定性也有回声",
        "最后一行仍在前行",
    };
    const size_t count = sizeof(offsets) / sizeof(offsets[0]);
    const size_t bytes = offsetof(music_lyric_snapshot_t, lines) +
                         count * sizeof(music_lyric_line_t);
    music_lyric_snapshot_t *lyric =
        music_snapshot_alloc(bytes, NULL);
    if (lyric == NULL)
    {
        return NULL;
    }
    lyric->generation = SIM_MUSIC_FIXTURE_GENERATION;
    lyric->line_count = count;
    for (size_t index = 0U; index < count; ++index)
    {
        lyric->lines[index].offset_ms = offsets[index];
        (void)snprintf(lyric->lines[index].text,
                       sizeof(lyric->lines[index].text), "%s", texts[index]);
    }
    return lyric;
}

static void _draw_finder(uint8_t *modules, uint16_t size, uint16_t left,
                         uint16_t top)
{
    for (uint16_t y = 0U; y < 7U; ++y)
    {
        for (uint16_t x = 0U; x < 7U; ++x)
        {
            const bool edge = x == 0U || x == 6U || y == 0U || y == 6U;
            const bool center = x >= 2U && x <= 4U &&
                                y >= 2U && y <= 4U;
            modules[(size_t)(top + y) * size + left + x] =
                edge || center ? 1U : 0U;
        }
    }
    for (uint16_t y = 0U; y < 8U; ++y)
    {
        for (uint16_t x = 0U; x < 8U; ++x)
        {
            if ((left == 1U && x == 7U &&
                    top + y < size) ||
                    (top == 1U && y == 7U &&
                     left + x < size))
            {
                const size_t pos = (size_t)(top + y) * size + left + x;
                if (pos < (size_t)size * size)
                {
                    modules[pos] = 0U;
                }
            }
        }
    }
}

static music_image_t *_create_qr(music_login_type_t type)
{
    const uint16_t size = SIM_MUSIC_QR_SIZE;
    const size_t pixel_bytes = (size_t)size * size * sizeof(uint16_t);
    music_image_t *image = music_snapshot_alloc(
                               sizeof(*image) + pixel_bytes, NULL);
    if (image == NULL)
    {
        return NULL;
    }
    uint16_t *pixels = (uint16_t *)(image + 1);
    uint8_t modules[SIM_MUSIC_QR_SIZE * SIM_MUSIC_QR_SIZE];
    memset(modules, 0, sizeof(modules));
    _draw_finder(modules, size, 1U, 1U);
    _draw_finder(modules, size, size - 8U, 1U);
    _draw_finder(modules, size, 1U, size - 8U);
    uint32_t random = 0x5EED0000U | (uint32_t)type;
    for (uint16_t y = 0U; y < size; ++y)
    {
        for (uint16_t x = 0U; x < size; ++x)
        {
            const bool finder = (x < 9U && y < 9U) ||
                                (x >= size - 9U && y < 9U) ||
                                (x < 9U && y >= size - 9U);
            if (!finder && !(x == 6U || y == 6U))
            {
                random ^= random << 13U;
                random ^= random >> 17U;
                random ^= random << 5U;
                modules[(size_t)y * size + x] =
                    (uint8_t)(random & 1U);
            }
        }
    }
    for (size_t index = 0U; index < (size_t)size * size; ++index)
    {
        pixels[index] = modules[index] != 0U ? UINT16_C(0x0000) :
                        UINT16_C(0xFFFF);
    }
    image->width = size;
    image->height = size;
    image->requested_width = size;
    image->requested_height = size;
    image->data_size = pixel_bytes;
    image->pixels = (uint8_t *)pixels;
    image->token = 0x51520001U;
    (void)snprintf(image->url, sizeof(image->url), "sim://music/login/%s",
                   type == MUSIC_LOGIN_TYPE_QQ ? "qq" : "wechat");
    return image;
}

static void _copy_queue(sim_music_fixture_t *fixture,
                        const music_playlist_snapshot_t *playlist,
                        uint16_t start_index)
{
    if (playlist == NULL || playlist->count == 0U ||
            start_index >= playlist->count)
    {
        return;
    }
    fixture->queue_count = playlist->count;
    fixture->queue_index = start_index;
    fixture->queue_valid = true;
    fixture->play_ref = playlist->playlist.ref;
    memcpy(fixture->queue, playlist->songs,
           (size_t)playlist->count * sizeof(fixture->queue[0]));
    (void)snprintf(fixture->lyric_songmid,
                   sizeof(fixture->lyric_songmid), "%s",
                   playlist->songs[start_index].songmid);
}

static void _set_player_from_queue(sim_music_fixture_t *fixture,
                                   music_playback_state_t state,
                                   uint32_t position_ms,
                                   music_service_failure_t failure)
{
    if (!fixture->queue_valid ||
            fixture->queue_index >= fixture->queue_count)
    {
        return;
    }
    const music_song_t *song = &fixture->queue[fixture->queue_index];
    music_player_status_t *player = &fixture->player;
    player->generation = SIM_MUSIC_FIXTURE_GENERATION;
    player->state = state;
    player->failure = failure;
    player->mode = MUSIC_PLAY_MODE_REPEAT_ALL;
    player->quality = MUSIC_QUALITY_HIGH;
    player->queue_index = fixture->queue_index;
    player->queue_count = fixture->queue_count;
    player->position_ms = position_ms < song->duration_ms ?
                          position_ms : song->duration_ms;
    player->duration_ms = song->duration_ms;
    player->buffered_ms = player->duration_ms;
    player->play_generation = SIM_MUSIC_FIXTURE_GENERATION;
    (void)snprintf(player->title, sizeof(player->title), "%s", song->title);
    (void)snprintf(player->artist, sizeof(player->artist), "%s",
                   song->artist);
    (void)snprintf(player->songmid, sizeof(player->songmid), "%s",
                   song->songmid);
    (void)snprintf(player->cover_url, sizeof(player->cover_url), "%s",
                   song->cover_url);
    fixture->play_valid = true;
}

static void _init_fixture_base(sim_music_fixture_t *fixture,
                               const char *name,
                               music_service_state_t state,
                               music_service_failure_t failure)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->name = name;
    fixture->state = state;
    fixture->failure = failure;
    fixture->player.generation = SIM_MUSIC_FIXTURE_GENERATION;
    fixture->player.state = MUSIC_PLAYBACK_IDLE;
    fixture->player.failure = MUSIC_SERVICE_FAILURE_NONE;
    fixture->player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    fixture->player.quality = MUSIC_QUALITY_HIGH;
}

static esp_err_t _prepare_catalog(const char *name,
                                  music_service_failure_t failure,
                                  music_playback_state_t player_state,
                                  uint16_t count, uint32_t total,
                                  uint32_t position_ms,
                                  sim_music_fixture_t *fixture)
{
    _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_READY, failure);
    fixture->home = _create_home(total);
    fixture->playlist = _create_playlist(count, total);
    fixture->lyric = _create_lyric();
    if (fixture->home == NULL || fixture->playlist == NULL ||
            fixture->lyric == NULL)
    {
        _release_fixture(fixture);
        return ESP_ERR_NO_MEM;
    }
    _copy_queue(fixture, fixture->playlist, 0U);
    _set_player_from_queue(fixture, player_state, position_ms, failure);
    return ESP_OK;
}

static esp_err_t _prepare_fixture(const char *name,
                                  sim_music_fixture_t *fixture)
{
    if (strcmp(name, "empty") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_LOGGED_OUT,
                           MUSIC_SERVICE_FAILURE_NONE);
        return ESP_OK;
    }
    if (strcmp(name, "catalog_playing") == 0)
    {
        return _prepare_catalog(name, MUSIC_SERVICE_FAILURE_NONE,
                                MUSIC_PLAYBACK_PLAYING,
                                SIM_MUSIC_CATALOG_COUNT,
                                SIM_MUSIC_CATALOG_COUNT, 42000U, fixture);
    }
    if (strcmp(name, "catalog_paused") == 0)
    {
        return _prepare_catalog(name, MUSIC_SERVICE_FAILURE_NONE,
                                MUSIC_PLAYBACK_PAUSED,
                                SIM_MUSIC_CATALOG_COUNT,
                                SIM_MUSIC_CATALOG_COUNT, 42000U, fixture);
    }
    if (strcmp(name, "catalog_buffering") == 0)
    {
        return _prepare_catalog(name, MUSIC_SERVICE_FAILURE_NONE,
                                MUSIC_PLAYBACK_BUFFERING,
                                SIM_MUSIC_CATALOG_COUNT,
                                SIM_MUSIC_CATALOG_COUNT, 8000U, fixture);
    }
    if (strcmp(name, "catalog_error") == 0)
    {
        return _prepare_catalog(name, MUSIC_SERVICE_FAILURE_DECODE,
                                MUSIC_PLAYBACK_ERROR,
                                SIM_MUSIC_CATALOG_COUNT,
                                SIM_MUSIC_CATALOG_COUNT, 42000U, fixture);
    }
    if (strcmp(name, "catalog_partial") == 0)
    {
        return _prepare_catalog(name, MUSIC_SERVICE_FAILURE_NONE,
                                MUSIC_PLAYBACK_PAUSED,
                                SIM_MUSIC_PARTIAL_COUNT,
                                SIM_MUSIC_PARTIAL_TOTAL, 12000U, fixture);
    }
    if (strcmp(name, "catalog_long") == 0)
    {
        return _prepare_catalog(name, MUSIC_SERVICE_FAILURE_NONE,
                                MUSIC_PLAYBACK_PAUSED,
                                SIM_MUSIC_LONG_COUNT,
                                SIM_MUSIC_LONG_TOTAL, 21000U, fixture);
    }
    if (strcmp(name, "catalog_long_lyric") == 0)
    {
        const esp_err_t result = _prepare_catalog(
                                     name, MUSIC_SERVICE_FAILURE_NONE,
                                     MUSIC_PLAYBACK_PLAYING,
                                     SIM_MUSIC_CATALOG_COUNT,
                                     SIM_MUSIC_CATALOG_COUNT, 42000U,
                                     fixture);
        if (result != ESP_OK)
        {
            return result;
        }
        if (fixture->lyric != NULL && fixture->lyric->line_count > 2U)
        {
            (void)snprintf(fixture->lyric->lines[2].text,
                           sizeof(fixture->lyric->lines[2].text),
                           "%s", "当坊间最善舞的女儿死了这一句故意写得很长");
        }
        return ESP_OK;
    }
    if (strcmp(name, "catalog_pending") == 0 ||
            strcmp(name, "catalog_idle") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_READY,
                           MUSIC_SERVICE_FAILURE_NONE);
        fixture->home = _create_home(SIM_MUSIC_CATALOG_COUNT);
        if (fixture->home == NULL)
        {
            _release_fixture(fixture);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (strcmp(name, "catalog_no_collected") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_READY,
                           MUSIC_SERVICE_FAILURE_NONE);
        fixture->home = _create_home(SIM_MUSIC_CATALOG_COUNT);
        if (fixture->home == NULL)
        {
            _release_fixture(fixture);
            return ESP_ERR_NO_MEM;
        }
        fixture->home->collected_ok = false;
        fixture->home->my_collected_count = 0U;
        return ESP_OK;
    }
    if (strcmp(name, "waiting_network") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_WAITING_NETWORK,
                           MUSIC_SERVICE_FAILURE_NONE);
        return ESP_OK;
    }
    if (strcmp(name, "logged_out") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_LOGGED_OUT,
                           MUSIC_SERVICE_FAILURE_NONE);
        return ESP_OK;
    }
    if (strcmp(name, "login_waiting") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_QR_READY,
                           MUSIC_SERVICE_FAILURE_NONE);
        fixture->login_qr = _create_qr(MUSIC_LOGIN_TYPE_QQ);
        if (fixture->login_qr == NULL)
        {
            _release_fixture(fixture);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (strcmp(name, "login_error") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_ERROR,
                           MUSIC_SERVICE_FAILURE_AUTH);
        return ESP_OK;
    }
    if (strcmp(name, "login_expired") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_QR_EXPIRED,
                           MUSIC_SERVICE_FAILURE_AUTH);
        fixture->login_qr = _create_qr(MUSIC_LOGIN_TYPE_QQ);
        if (fixture->login_qr == NULL)
        {
            _release_fixture(fixture);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (strcmp(name, "service_error") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_ERROR,
                           MUSIC_SERVICE_FAILURE_API);
        return ESP_OK;
    }
    if (strcmp(name, "player_unavailable") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_ERROR,
                           MUSIC_SERVICE_FAILURE_API);
        fixture->player_unavailable = true;
        return ESP_OK;
    }
    if (strcmp(name, "catalog_fail_twice") == 0)
    {
        const esp_err_t result = _prepare_catalog(
                                     name, MUSIC_SERVICE_FAILURE_NONE,
                                     MUSIC_PLAYBACK_PAUSED,
                                     SIM_MUSIC_LONG_COUNT,
                                     SIM_MUSIC_LONG_TOTAL, 21000U, fixture);
        if (result == ESP_OK)
        {
            fixture->playlist_failures_remaining = 2U;
        }
        return result;
    }
    if (strcmp(name, "playlist_failed") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_READY,
                           MUSIC_SERVICE_FAILURE_NONE);
        fixture->home = _create_home(SIM_MUSIC_CATALOG_COUNT);
        fixture->playlist_failed = true;
        return ESP_OK;
    }
    if (strcmp(name, "queue_failed") == 0)
    {
        _init_fixture_base(fixture, name, MUSIC_SERVICE_STATE_READY,
                           MUSIC_SERVICE_FAILURE_NONE);
        fixture->queue_failed = true;
        return ESP_OK;
    }
    if (strcmp(name, "lyric_failed") == 0)
    {
        const esp_err_t result = _prepare_catalog(
                                     name, MUSIC_SERVICE_FAILURE_NONE,
                                     MUSIC_PLAYBACK_PLAYING,
                                     SIM_MUSIC_CATALOG_COUNT,
                                     SIM_MUSIC_CATALOG_COUNT, 42000U,
                                     fixture);
        if (result != ESP_OK)
        {
            return result;
        }
        (void)snprintf(fixture->player.songmid,
                       sizeof(fixture->player.songmid), "%s", "failed-track");
        fixture->lyric_failed = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static void _apply_fixture(const sim_music_fixture_t *fixture)
{
    music_home_snapshot_t *old_home;
    music_playlist_snapshot_t *old_playlist;
    music_lyric_snapshot_t *old_lyric;
    music_image_t *old_qr;
    music_image_t *old_image;

    (void)pthread_mutex_lock(&s_music.lock);
    _release_state_locked(&s_music, &old_home, &old_playlist, &old_lyric,
                          &old_qr, &old_image);
    s_music.fixture = fixture->name;
    s_music.generation = s_music.generation + 1U;
    if (s_music.generation == 0U)
    {
        s_music.generation = 1U;
    }
    memset(&s_music.status, 0, sizeof(s_music.status));
    s_music.status.generation = s_music.generation;
    s_music.status.state = fixture->state;
    s_music.status.failure = fixture->failure;
    s_music.status.network_ready = true;
    s_music.status.time_ready = true;
    s_music.status.audio_available = true;
    music_service_status_t *status = &s_music.status;
    status->logged_in = fixture->state == MUSIC_SERVICE_STATE_READY;
    status->qr_available = fixture->login_qr != NULL;
    status->login_type = MUSIC_LOGIN_TYPE_QQ;
    if (fixture->state == MUSIC_SERVICE_STATE_QR_READY)
    {
        (void)snprintf(status->qr_status, sizeof(status->qr_status),
                       "请使用QQ扫码");
    }
    else if (strcmp(fixture->name, "login_expired") == 0)
    {
        (void)snprintf(status->qr_status, sizeof(status->qr_status),
                       "二维码已过期，请重新登录");
    }
    else if (fixture->failure == MUSIC_SERVICE_FAILURE_AUTH)
    {
        (void)snprintf(status->qr_status, sizeof(status->qr_status),
                       "登录失败，请重试");
    }
    else if (fixture->failure == MUSIC_SERVICE_FAILURE_API)
    {
        (void)snprintf(status->qr_status, sizeof(status->qr_status),
                       "音乐服务暂不可用");
    }
    if (status->logged_in)
    {
        (void)snprintf(status->user_name, sizeof(status->user_name), "%s",
                       "模拟用户");
    }

    s_music.player = fixture->player;
    s_music.player.generation = s_music.generation;
    s_music.status.generation = s_music.generation;
    if (fixture->home != NULL)
    {
        fixture->home->generation = s_music.generation;
    }
    if (fixture->playlist != NULL)
    {
        fixture->playlist->generation = s_music.generation;
    }
    if (fixture->lyric != NULL)
    {
        fixture->lyric->generation = s_music.generation;
    }
    s_music.player_unavailable = fixture->player_unavailable;
    s_music.playlist_failed = fixture->playlist_failed;
    s_music.playlist_failures_remaining =
        fixture->playlist_failures_remaining;
    s_music.queue_failed = fixture->queue_failed;
    s_music.lyric_failed = fixture->lyric_failed;
    s_music.home = fixture->home;
    s_music.playlist = fixture->playlist;
    s_music.lyric = fixture->lyric;
    s_music.login_qr = fixture->login_qr;
    s_music.play_ref = fixture->play_ref;
    memcpy(s_music.queue, fixture->queue, sizeof(s_music.queue));
    s_music.queue_count = fixture->queue_count;
    s_music.queue_index = fixture->queue_index;
    s_music.queue_valid = fixture->queue_valid;
    s_music.play_valid = fixture->play_valid;
    (void)snprintf(s_music.lyric_songmid, sizeof(s_music.lyric_songmid),
                   "%s", fixture->lyric_songmid);
    (void)pthread_mutex_unlock(&s_music.lock);

    _release_detached(old_home, old_playlist, old_lyric, old_qr, old_image);
}

esp_err_t host_music_service_set_fixture(const char *name)
{
    if (name == NULL || name[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    sim_music_fixture_t fixture;
    const esp_err_t result = _prepare_fixture(name, &fixture);
    if (result != ESP_OK)
    {
        return result;
    }
    _apply_fixture(&fixture);
    memset(&fixture, 0, sizeof(fixture));
    _publish_fixture();
    return ESP_OK;
}

esp_err_t host_music_service_get_fixture_status(
    sim_music_fixture_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    (void)pthread_mutex_lock(&s_music.lock);
    status->fixture = s_music.fixture;
    status->generation = s_music.generation;
    status->state = (uint32_t)s_music.status.state;
    status->failure = (uint32_t)s_music.status.failure;
    status->player_state = (uint32_t)s_music.player.state;
    if (s_music.home != NULL)
    {
        status->home_count = s_music.home->my_created_count +
                             s_music.home->my_favorite_count +
                             s_music.home->recommended_count +
                             s_music.home->my_collected_count;
    }
    if (s_music.playlist != NULL)
    {
        status->playlist_count = s_music.playlist->count;
    }
    if (s_music.lyric != NULL)
    {
        status->lyric_lines = s_music.lyric->line_count;
    }
    status->qr_available = s_music.login_qr != NULL;
    (void)pthread_mutex_unlock(&s_music.lock);
    return ESP_OK;
}

esp_err_t host_music_service_login_scan(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.status.state != MUSIC_SERVICE_STATE_QR_READY ||
            s_music.login_qr == NULL)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_music.status.state = MUSIC_SERVICE_STATE_QR_SCANNED;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    (void)snprintf(s_music.status.qr_status,
                   sizeof(s_music.status.qr_status),
                   "已扫码，请在手机上确认");
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t host_music_service_login_confirm(void)
{
    music_image_t *old_qr = NULL;
    music_home_snapshot_t *home = NULL;
    music_playlist_snapshot_t *playlist = NULL;
    music_lyric_snapshot_t *lyric = NULL;

    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.status.state != MUSIC_SERVICE_STATE_QR_SCANNED &&
            s_music.status.state != MUSIC_SERVICE_STATE_QR_READY &&
            s_music.status.state != MUSIC_SERVICE_STATE_AUTHENTICATING)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.home == NULL)
    {
        home = _create_home(SIM_MUSIC_CATALOG_COUNT);
    }
    if (s_music.playlist == NULL)
    {
        playlist = _create_playlist(SIM_MUSIC_CATALOG_COUNT,
                                    SIM_MUSIC_CATALOG_COUNT);
    }
    if (s_music.lyric == NULL)
    {
        lyric = _create_lyric();
    }
    if (s_music.home == NULL && home == NULL)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NO_MEM;
    }
    old_qr = s_music.login_qr;
    s_music.login_qr = NULL;
    s_music.status.state = MUSIC_SERVICE_STATE_READY;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.logged_in = true;
    s_music.status.qr_available = false;
    s_music.status.qr_status[0] = '\0';
    (void)snprintf(s_music.status.user_name,
                   sizeof(s_music.status.user_name), "模拟用户");
    if (home != NULL)
    {
        s_music.home = home;
    }
    if (playlist != NULL)
    {
        s_music.playlist = playlist;
    }
    if (lyric != NULL)
    {
        s_music.lyric = lyric;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    music_snapshot_release(old_qr);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_init(const music_service_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t music_service_deinit(void)
{
    music_home_snapshot_t *home;
    music_playlist_snapshot_t *playlist;
    music_lyric_snapshot_t *lyric;
    music_image_t *qr;
    music_image_t *image;

    (void)pthread_mutex_lock(&s_music.lock);
    _release_state_locked(&s_music, &home, &playlist, &lyric, &qr, &image);
    memset(&s_music.status, 0, sizeof(s_music.status));
    memset(&s_music.player, 0, sizeof(s_music.player));
    memset(&s_music.queue, 0, sizeof(s_music.queue));
    s_music.queue_count = 0U;
    s_music.queue_index = 0U;
    s_music.queue_valid = false;
    s_music.play_valid = false;
    s_music.lyric_songmid[0] = '\0';
    s_music.status.state = MUSIC_SERVICE_STATE_UNINITIALIZED;
    (void)pthread_mutex_unlock(&s_music.lock);
    _release_detached(home, playlist, lyric, qr, image);
    return ESP_OK;
}

esp_err_t music_service_set_network_ready(bool ready)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.status.network_ready = ready;
    if (!ready && s_music.status.state != MUSIC_SERVICE_STATE_ERROR)
    {
        s_music.status.state = MUSIC_SERVICE_STATE_WAITING_NETWORK;
        s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t music_service_set_time_ready(bool ready)
{
    (void)pthread_mutex_lock(&s_music.lock);
    s_music.status.time_ready = ready;
    if (!ready && s_music.status.state != MUSIC_SERVICE_STATE_ERROR &&
            s_music.status.state != MUSIC_SERVICE_STATE_WAITING_NETWORK)
    {
        s_music.status.state = MUSIC_SERVICE_STATE_WAITING_TIME;
        s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
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
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t music_service_login_begin(music_login_type_t type)
{
    if (type != MUSIC_LOGIN_TYPE_WECHAT && type != MUSIC_LOGIN_TYPE_QQ)
    {
        return ESP_ERR_INVALID_ARG;
    }
    music_image_t *qr = _create_qr(type);
    if (qr == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    music_image_t *old_qr = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    if (!s_music.status.network_ready || !s_music.status.time_ready)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        music_snapshot_release(qr);
        return ESP_ERR_INVALID_STATE;
    }
    old_qr = s_music.login_qr;
    s_music.login_qr = qr;
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
    music_snapshot_release(old_qr);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t music_service_login_qr_acquire(const music_image_t **image)
{
    if (image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    music_image_t *qr = music_snapshot_retain(s_music.login_qr);
    (void)pthread_mutex_unlock(&s_music.lock);
    if (qr == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *image = qr;
    return ESP_OK;
}

void music_service_login_qr_release(const music_image_t *image)
{
    music_snapshot_release((void *)image);
}

esp_err_t music_service_login_cancel(void)
{
    music_image_t *old_qr = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    old_qr = s_music.login_qr;
    s_music.login_qr = NULL;
    if (s_music.status.state == MUSIC_SERVICE_STATE_QR_READY ||
            s_music.status.state == MUSIC_SERVICE_STATE_QR_SCANNED ||
            s_music.status.state == MUSIC_SERVICE_STATE_QR_EXPIRED ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTHENTICATING ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTH_REQUIRED)
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
    music_snapshot_release(old_qr);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t music_service_logout(void)
{
    music_home_snapshot_t *home;
    music_playlist_snapshot_t *playlist;
    music_lyric_snapshot_t *lyric;
    music_image_t *qr;
    music_image_t *image;

    (void)pthread_mutex_lock(&s_music.lock);
    _release_state_locked(&s_music, &home, &playlist, &lyric, &qr, &image);
    s_music.status.logged_in = false;
    s_music.status.qr_available = false;
    s_music.status.state = MUSIC_SERVICE_STATE_LOGGED_OUT;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.user_name[0] = '\0';
    s_music.status.user_avatar[0] = '\0';
    s_music.status.qr_status[0] = '\0';
    memset(&s_music.player, 0, sizeof(s_music.player));
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    s_music.player.quality = MUSIC_QUALITY_HIGH;
    memset(&s_music.queue, 0, sizeof(s_music.queue));
    s_music.queue_count = 0U;
    s_music.queue_index = 0U;
    s_music.queue_valid = false;
    s_music.play_valid = false;
    s_music.lyric_songmid[0] = '\0';
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _release_detached(home, playlist, lyric, qr, image);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_reset_persisted_state(void)
{
    return ESP_OK;
}

esp_err_t music_service_refresh_home(bool force)
{
    (void)force;
    (void)pthread_mutex_lock(&s_music.lock);
    const music_service_state_t state = s_music.status.state;
    (void)pthread_mutex_unlock(&s_music.lock);
    if (state == MUSIC_SERVICE_STATE_ERROR)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
    return ESP_OK;
}

esp_err_t music_service_home_acquire(const music_home_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    music_home_snapshot_t *home = music_snapshot_retain(s_music.home);
    (void)pthread_mutex_unlock(&s_music.lock);
    if (home == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = home;
    return ESP_OK;
}

void music_service_home_release(const music_home_snapshot_t *snapshot)
{
    music_snapshot_release((void *)snapshot);
}

static const music_playlist_t *_find_home_playlist(
    const music_home_snapshot_t *home, const music_playlist_ref_t *ref)
{
    if (home == NULL || ref == NULL)
    {
        return NULL;
    }
    const music_playlist_t *sections[] = {home->recommended, home->my_favorite,
                                          home->my_created, home->my_collected
                                         };
    const size_t counts[] = {home->recommended_count, home->my_favorite_count,
                             home->my_created_count, home->my_collected_count
                            };
    for (size_t section = 0U;
            section < sizeof(sections) / sizeof(sections[0]); ++section)
    {
        for (size_t index = 0U; index < counts[section]; ++index)
        {
            const music_playlist_ref_t *candidate = &sections[section][index].ref;
            if (candidate->id == ref->id &&
                    candidate->dirid == ref->dirid &&
                    candidate->tid == ref->tid &&
                    candidate->kind == ref->kind)
            {
                return &sections[section][index];
            }
        }
    }
    return NULL;
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
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.playlist_failed)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.playlist_failures_remaining > 0U)
    {
        --s_music.playlist_failures_remaining;
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    music_playlist_snapshot_t *playlist = s_music.playlist;
    const music_playlist_t *metadata = _find_home_playlist(s_music.home, ref);
    const bool matches = playlist != NULL && (ref == NULL || metadata != NULL);
    if (matches)
    {
        const uint32_t total = metadata != NULL ?
                               metadata->track_count : playlist->total;
        if (offset >= total)
        {
            (void)pthread_mutex_unlock(&s_music.lock);
            return ESP_ERR_NOT_FOUND;
        }
        uint32_t remaining = total - offset;
        uint16_t slice_count = count;
        if (remaining < slice_count)
        {
            slice_count = (uint16_t)remaining;
        }
        if (offset == 0U && playlist->offset == 0U &&
                playlist->count < slice_count)
        {
            slice_count = playlist->count;
        }
        const size_t bytes = offsetof(music_playlist_snapshot_t, songs) +
                             (size_t)slice_count * sizeof(music_song_t);
        music_playlist_snapshot_t *slice =
            music_snapshot_alloc(bytes, NULL);
        if (slice == NULL)
        {
            (void)pthread_mutex_unlock(&s_music.lock);
            return ESP_ERR_NO_MEM;
        }
        slice->generation = playlist->generation;
        if (metadata != NULL)
        {
            slice->playlist = *metadata;
        }
        else
        {
            slice->playlist = playlist->playlist;
        }
        slice->total = total;
        slice->offset = offset;
        slice->count = slice_count;
        for (uint16_t index = 0U; index < slice_count; ++index)
        {
            const uint32_t source_index = (uint32_t)offset + index;
            if (source_index < playlist->count)
            {
                slice->songs[index] = playlist->songs[source_index];
            }
            else
            {
                _fill_song(&slice->songs[index], (uint16_t)source_index);
            }
        }
        (void)pthread_mutex_unlock(&s_music.lock);
        *snapshot = slice;
        return ESP_OK;
    }
    (void)pthread_mutex_unlock(&s_music.lock);
    return ESP_ERR_NOT_FOUND;
}

void music_service_playlist_release(const music_playlist_snapshot_t *snapshot)
{
    music_snapshot_release((void *)snapshot);
}

esp_err_t music_service_lyric_acquire(const char *songmid,
                                      const music_lyric_snapshot_t **snapshot)
{
    if (songmid == NULL || songmid[0] == '\0' || snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.lyric_failed)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    music_lyric_snapshot_t *lyric = NULL;
    if (s_music.lyric != NULL &&
            strcmp(s_music.lyric_songmid, songmid) == 0)
    {
        lyric = music_snapshot_retain(s_music.lyric);
    }
    (void)pthread_mutex_unlock(&s_music.lock);
    if (lyric == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = lyric;
    return ESP_OK;
}

void music_service_lyric_release(const music_lyric_snapshot_t *snapshot)
{
    music_snapshot_release((void *)snapshot);
}

static music_image_t *_create_cover(const char *url, uint16_t width,
                                    uint16_t height, uint32_t token)
{
    const size_t pixel_bytes = (size_t)width * height * sizeof(uint16_t);
    music_image_t *image = music_snapshot_alloc(sizeof(*image) + pixel_bytes,
        NULL);
    if (image == NULL)
    {
        return NULL;
    }
    uint16_t *pixels = (uint16_t *)(image + 1);
    const uint32_t seed = token ^ (uint32_t)width ^ ((uint32_t)height << 16U);
    for (uint16_t y = 0U; y < height; ++y)
    {
        for (uint16_t x = 0U; x < width; ++x)
        {
            const uint16_t shade = (uint16_t)(((uint32_t)x * 2U + y) * 32U /
                                              ((uint32_t)width * 2U +
                                               height));
            uint16_t red = (uint16_t)(1U + shade / 6U);
            uint16_t green = (uint16_t)(8U + shade / 2U +
                                        ((seed >> 5U) & 3U));
            uint16_t blue = (uint16_t)(14U + shade + ((seed >> 9U) & 7U));
            if (((x + (uint32_t)y * 2U + (seed & 31U)) % 97U) < 6U)
            {
                green = (uint16_t)(green + 10U);
                blue = (uint16_t)(blue + 6U);
            }
            pixels[(size_t)y * width + x] =
                (uint16_t)((red << 11U) | (green << 5U) | blue);
        }
    }
    image->width = width;
    image->height = height;
    image->requested_width = width;
    image->requested_height = height;
    image->data_size = pixel_bytes;
    image->pixels = (uint8_t *)pixels;
    image->token = token;
    (void)snprintf(image->url, sizeof(image->url), "%s", url);
    return image;
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
    music_image_t *old = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.image != NULL && s_music.image->token == token &&
            s_music.image->requested_width == width &&
            s_music.image->requested_height == height &&
            strcmp(s_music.image->url, url) == 0)
    {
        *image = music_snapshot_retain(s_music.image);
        (void)pthread_mutex_unlock(&s_music.lock);
        return *image != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    music_image_t *completed = _create_cover(url, width, height, token);
    if (completed == NULL)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NO_MEM;
    }
    old = s_music.image;
    s_music.image = completed;
    (void)pthread_mutex_unlock(&s_music.lock);
    music_snapshot_release(old);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE, token);
    return ESP_ERR_NOT_FOUND;
}

void music_service_image_release(const music_image_t *image)
{
    music_snapshot_release((void *)image);
}

static void _select_queue_song_locked(uint16_t index)
{
    s_music.queue_index = index;
    s_music.play_valid = true;
    const music_song_t *song = &s_music.queue[index];
    s_music.player.queue_index = index;
    s_music.player.queue_count = s_music.queue_count;
    s_music.player.position_ms = 0U;
    s_music.player.duration_ms = song->duration_ms;
    s_music.player.buffered_ms = song->duration_ms;
    s_music.player.play_generation = s_music.generation;
    (void)snprintf(s_music.player.title, sizeof(s_music.player.title), "%s",
                   song->title);
    (void)snprintf(s_music.player.artist, sizeof(s_music.player.artist), "%s",
                   song->artist);
    (void)snprintf(s_music.player.songmid, sizeof(s_music.player.songmid), "%s",
                   song->songmid);
    (void)snprintf(s_music.lyric_songmid, sizeof(s_music.lyric_songmid), "%s",
                   song->songmid);
    (void)snprintf(s_music.player.cover_url, sizeof(s_music.player.cover_url),
                   "%s", song->cover_url);
}

esp_err_t music_service_queue_acquire(const music_queue_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.queue_failed)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_music.queue_valid || s_music.queue_count == 0U)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NOT_FOUND;
    }
    const size_t bytes = offsetof(music_queue_snapshot_t, songs) +
                         (size_t)s_music.queue_count *
                         sizeof(s_music.queue[0]);
    music_queue_snapshot_t *queue = music_snapshot_alloc(bytes, NULL);
    if (queue == NULL)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NO_MEM;
    }
    queue->generation = s_music.generation;
    queue->count = s_music.queue_count;
    memcpy(queue->songs, s_music.queue,
           (size_t)s_music.queue_count * sizeof(s_music.queue[0]));
    (void)pthread_mutex_unlock(&s_music.lock);
    *snapshot = queue;
    return ESP_OK;
}

void music_service_queue_release(const music_queue_snapshot_t *snapshot)
{
    music_snapshot_release((void *)snapshot);
}

esp_err_t music_service_play_playlist(const music_playlist_ref_t *ref,
                                      uint16_t start_index)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (s_music.playlist == NULL || s_music.playlist->count == 0U)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_NOT_FOUND;
    }
    const music_playlist_t *metadata =
        _find_home_playlist(s_music.home, ref);
    const bool matches = ref == NULL || metadata != NULL;
    if (!matches || start_index >= s_music.playlist->count)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_music.queue, s_music.playlist->songs,
           (size_t)s_music.playlist->count * sizeof(s_music.queue[0]));
    s_music.queue_count = s_music.playlist->count;
    s_music.play_ref = ref != NULL ? *ref : s_music.playlist->playlist.ref;
    s_music.queue_valid = true;
    _set_generation_locked();
    _select_queue_song_locked(start_index);
    s_music.player.state = MUSIC_PLAYBACK_PLAYING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.play_generation = s_music.generation;
    s_music.player.generation = s_music.generation;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
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
    if (!s_music.queue_valid || index >= s_music.queue_count)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_ARG;
    }
    _set_generation_locked();
    _select_queue_song_locked(index);
    s_music.player.state = MUSIC_PLAYBACK_PLAYING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.generation = s_music.generation;
    s_music.player.play_generation = s_music.generation;
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_queue_remove(uint16_t index)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (!s_music.queue_valid || index >= s_music.queue_count)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t old_index = s_music.queue_index;
    const uint16_t old_count = s_music.queue_count;
    const bool playing = s_music.player.state == MUSIC_PLAYBACK_PLAYING ||
                         s_music.player.state == MUSIC_PLAYBACK_BUFFERING;
    if (index + 1U < old_count)
    {
        memmove(&s_music.queue[index], &s_music.queue[index + 1U],
                (size_t)(old_count - index - 1U) *
                sizeof(s_music.queue[0]));
    }
    s_music.queue_count = (uint16_t)(old_count - 1U);
    uint16_t new_index = old_index;
    if (s_music.queue_count == 0U)
    {
        new_index = 0U;
        s_music.queue_valid = false;
        s_music.play_valid = false;
        memset(&s_music.player, 0, sizeof(s_music.player));
        s_music.player.state = MUSIC_PLAYBACK_IDLE;
        s_music.player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
        s_music.player.quality = MUSIC_QUALITY_HIGH;
    }
    else
    {
        if (index < old_index)
        {
            new_index = (uint16_t)(old_index - 1U);
        }
        else if (index == old_index)
        {
            new_index = old_index < s_music.queue_count ? old_index :
                        (uint16_t)(s_music.queue_count - 1U);
        }
        s_music.queue_index = new_index;
        if (index == old_index && playing)
        {
            _select_queue_song_locked(new_index);
            s_music.player.state = MUSIC_PLAYBACK_PLAYING;
            s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
        }
        else
        {
            s_music.player.queue_index = new_index;
            s_music.player.queue_count = s_music.queue_count;
        }
    }
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_play_pause(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (!s_music.play_valid || s_music.player.title[0] == '\0' ||
            (s_music.player.state != MUSIC_PLAYBACK_PLAYING &&
             s_music.player.state != MUSIC_PLAYBACK_PAUSED))
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_music.player.state =
        s_music.player.state == MUSIC_PLAYBACK_PLAYING ?
        MUSIC_PLAYBACK_PAUSED : MUSIC_PLAYBACK_PLAYING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    _set_generation_locked();
    s_music.player.play_generation = s_music.generation;
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

static esp_err_t _step_queue(bool forward)
{
    (void)pthread_mutex_lock(&s_music.lock);
    if (!s_music.queue_valid || !s_music.play_valid ||
            s_music.queue_count == 0U ||
            s_music.player.state == MUSIC_PLAYBACK_IDLE ||
            s_music.player.state == MUSIC_PLAYBACK_ERROR)
    {
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t next = s_music.queue_index;
    if (s_music.player.mode == MUSIC_PLAY_MODE_REPEAT_ONE)
    {
        if (!forward)
        {
            (void)pthread_mutex_unlock(&s_music.lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    else if (s_music.player.mode == MUSIC_PLAY_MODE_REPEAT_ALL)
    {
        next = forward ? (uint16_t)((next + 1U) % s_music.queue_count) :
               (uint16_t)((next + s_music.queue_count - 1U) %
                          s_music.queue_count);
    }
    else if (s_music.player.mode == MUSIC_PLAY_MODE_SHUFFLE)
    {
        next = forward ? (uint16_t)((next + 1U) % s_music.queue_count) :
               (uint16_t)((next + s_music.queue_count - 1U) %
                          s_music.queue_count);
    }
    else if (forward)
    {
        if ((uint32_t)next + 1U >= s_music.queue_count)
        {
            (void)pthread_mutex_unlock(&s_music.lock);
            return ESP_ERR_INVALID_STATE;
        }
        ++next;
    }
    else
    {
        if (next == 0U)
        {
            (void)pthread_mutex_unlock(&s_music.lock);
            return ESP_ERR_INVALID_STATE;
        }
        --next;
    }
    _set_generation_locked();
    _select_queue_song_locked(next);
    s_music.player.state = MUSIC_PLAYBACK_PLAYING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.generation = s_music.generation;
    s_music.player.play_generation = s_music.generation;
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_next(void)
{
    return _step_queue(true);
}

esp_err_t music_service_prev(void)
{
    return _step_queue(false);
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
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_stop(void)
{
    (void)pthread_mutex_lock(&s_music.lock);
    memset(&s_music.player, 0, sizeof(s_music.player));
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.mode = MUSIC_PLAY_MODE_SEQUENTIAL;
    s_music.player.quality = MUSIC_QUALITY_HIGH;
    memset(&s_music.queue, 0, sizeof(s_music.queue));
    s_music.queue_count = 0U;
    s_music.queue_index = 0U;
    s_music.queue_valid = false;
    s_music.play_valid = false;
    s_music.lyric_songmid[0] = '\0';
    _set_generation_locked();
    (void)pthread_mutex_unlock(&s_music.lock);
    _publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
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
    if (s_music.player_unavailable)
    {
        memset(status, 0, sizeof(*status));
        (void)pthread_mutex_unlock(&s_music.lock);
        return ESP_ERR_INVALID_STATE;
    }
    *status = s_music.player;
    (void)pthread_mutex_unlock(&s_music.lock);
    return ESP_OK;
}

esp_err_t music_service_get_diagnostics(music_service_diagnostics_t *diagnostics)
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
    const bool playing = s_music.player.state == MUSIC_PLAYBACK_PLAYING ||
                         s_music.player.state == MUSIC_PLAYBACK_BUFFERING;
    (void)pthread_mutex_unlock(&s_music.lock);
    return playing;
}

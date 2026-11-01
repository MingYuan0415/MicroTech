/** @file Host CDN playback adapter for the simulator's real music backend.
 *
 * The device owns the production I2S/decoder path in music_stream.c. The
 * simulator deliberately substitutes this adapter: it downloads the real CDN
 * URL with libcurl, decodes the real MP3/FLAC container with libsndfile, and
 * feeds interleaved PCM into the simulator's fake audio service. The decoder
 * and network are real; only the speaker sink is simulated.
 */
#define _GNU_SOURCE

#include "music_service_internal.h"

#define DBG_TAG "sim_music_stream"
#include "audio_service.h"
#include "esp_err.h"
#include "mt_log.h"

#include <curl/curl.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim_sndfile.h"

#define SIM_STREAM_CHUNK_FRAMES 1152U
#define SIM_STREAM_MAX_CHANNELS 2U

typedef struct sim_sndfile_api
{
    void *handle;
    SNDFILE *(*open)(const char *, int, SF_INFO *);
    sf_count_t (*readf_short)(SNDFILE *, int16_t *, sf_count_t);
    int (*close)(SNDFILE *);
    const char *(*strerror)(SNDFILE *);
} sim_sndfile_api_t;

typedef struct sim_stream_command
{
    char url[MUSIC_SERVICE_URL_BYTES];
    uint32_t duration_ms;
    uint32_t generation;
} sim_stream_command_t;

typedef struct sim_stream
{
    bool initialized;
    bool sndfile_ready;
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_started;
    uint32_t active_generation;
    sim_sndfile_api_t sndfile;
    music_stream_status_t status;
    atomic_bool stop;
    atomic_bool pause;
    atomic_bool shutdown;
    atomic_int public_state;
} sim_stream_t;

static sim_stream_t s_stream;

static void _sndfile_close(sim_sndfile_api_t *api)
{
    if (api->handle != NULL)
    {
        (void)dlclose(api->handle);
    }
    memset(api, 0, sizeof(*api));
}

static bool _sndfile_open(sim_sndfile_api_t *api)
{
    memset(api, 0, sizeof(*api));
    api->handle = dlopen("libsndfile.so.1", RTLD_NOW | RTLD_LOCAL);
    if (api->handle == NULL)
    {
        api->handle = dlopen("libsndfile.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (api->handle == NULL)
    {
        return false;
    }
    *(void **)(&api->open) = dlsym(api->handle, "sf_open");
    *(void **)(&api->readf_short) = dlsym(api->handle, "sf_readf_short");
    *(void **)(&api->close) = dlsym(api->handle, "sf_close");
    *(void **)(&api->strerror) = dlsym(api->handle, "sf_strerror");
    if (api->open == NULL || api->readf_short == NULL ||
            api->close == NULL)
    {
        _sndfile_close(api);
        return false;
    }
    return true;
}

static void _set_status(music_playback_state_t state,
                        music_service_failure_t failure)
{
    atomic_store_explicit(&s_stream.public_state, (int)state,
                          memory_order_release);
    if (pthread_mutex_lock(&s_stream.lock) == 0)
    {
        s_stream.status.state = state;
        s_stream.status.failure = failure;
        pthread_mutex_unlock(&s_stream.lock);
    }
}

static void _set_position(uint32_t position_ms)
{
    if (pthread_mutex_lock(&s_stream.lock) == 0)
    {
        s_stream.status.position_ms = position_ms;
        pthread_mutex_unlock(&s_stream.lock);
    }
}

typedef struct sim_download
{
    FILE *file;
    atomic_bool *stop;
} sim_download_t;

static size_t _download_write(char *data, size_t size, size_t nitems,
                              void *user_data)
{
    sim_download_t *download = user_data;
    if (atomic_load_explicit(download->stop, memory_order_acquire))
    {
        return 0;
    }
    return fwrite(data, size, nitems, download->file);
}

static int _download_progress(void *user_data, curl_off_t dltotal,
                              curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    sim_download_t *download = user_data;
    return atomic_load_explicit(download->stop, memory_order_acquire) ? 1 : 0;
}

static bool _download_url(const char *url, char *path, size_t path_size,
                          uint32_t generation)
{
    (void)generation;
    const bool flac = strstr(url, ".flac") != NULL;
    (void)snprintf(path, path_size, "/tmp/mt-music-XXXXXX%s",
                   flac ? ".flac" : ".mp3");
    const int suffix_length = flac ? 5 : 4;
    const int fd = mkstemps(path, suffix_length);
    if (fd < 0)
    {
        return false;
    }
    FILE *file = fdopen(fd, "wb+");
    if (file == NULL)
    {
        close(fd);
        unlink(path);
        return false;
    }
    sim_download_t download =
    {
        .file = file,
        .stop = &s_stream.stop,
    };
    CURL *curl = curl_easy_init();
    if (curl == NULL)
    {
        fclose(file);
        unlink(path);
        return false;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
                                "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                "AppleWebKit/537.36 (KHTML, like Gecko) "
                                "Chrome/120.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers, "Referer: https://y.qq.com/");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _download_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &download);
    const CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    const bool ok = result == CURLE_OK && !atomic_load(&s_stream.stop);
    if (fflush(file) != 0 || fseek(file, 0L, SEEK_SET) != 0)
    {
        fclose(file);
        unlink(path);
        return false;
    }
    fclose(file);
    if (!ok)
    {
        unlink(path);
    }
    return ok;
}

static void *_stream_task(void *argument)
{
    sim_stream_command_t *command = argument;
    const uint32_t generation = command->generation;
    char path[64] = {0};
    SNDFILE *sndfile = NULL;
    bool audio_ready = false;
    bool failed = false;
    bool natural_end = false;
    bool stopped = false;
    uint32_t position_ms = 0U;

    _set_status(MUSIC_PLAYBACK_BUFFERING, MUSIC_SERVICE_FAILURE_NONE);
    if (!_download_url(command->url, path, sizeof(path), generation))
    {
        _set_status(MUSIC_PLAYBACK_ERROR, MUSIC_SERVICE_FAILURE_NETWORK);
        failed = true;
        goto cleanup;
    }
    if (!s_stream.sndfile_ready)
    {
        _set_status(MUSIC_PLAYBACK_ERROR, MUSIC_SERVICE_FAILURE_DECODE);
        failed = true;
        goto cleanup;
    }
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    sndfile = s_stream.sndfile.open(path, SFM_READ, &info);
    if (sndfile == NULL || info.samplerate <= 0 || info.channels <= 0 ||
            info.channels > (int)SIM_STREAM_MAX_CHANNELS)
    {
        LOG_E("sim music decode open failed: %s",
              sndfile != NULL && s_stream.sndfile.strerror != NULL ?
              s_stream.sndfile.strerror(sndfile) : "unsupported format");
        _set_status(MUSIC_PLAYBACK_ERROR, MUSIC_SERVICE_FAILURE_DECODE);
        failed = true;
        goto cleanup;
    }
    const audio_service_config_t config =
    {
        .sample_rate_hz = (uint32_t)info.samplerate,
        .bits_per_sample = 16U,
        .channels = (uint8_t)info.channels,
        .mclk_multiple = 256U,
    };
    if (audio_service_configure(&config) != ESP_OK)
    {
        _set_status(MUSIC_PLAYBACK_ERROR, MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        failed = true;
        goto cleanup;
    }
    audio_service_lease_t lease;
    if (audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &lease) !=
            ESP_OK)
    {
        _set_status(MUSIC_PLAYBACK_ERROR, MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        failed = true;
        goto cleanup;
    }
    audio_ready = true;
    pthread_mutex_lock(&s_stream.lock);
    s_stream.status.duration_ms = command->duration_ms != 0U ?
                                  command->duration_ms :
                                  (uint32_t)(info.frames * 1000U /
                                      (uint64_t)info.samplerate);
    pthread_mutex_unlock(&s_stream.lock);
    _set_status(MUSIC_PLAYBACK_PLAYING, MUSIC_SERVICE_FAILURE_NONE);

    int16_t *pcm = calloc(SIM_STREAM_CHUNK_FRAMES * info.channels,
                          sizeof(int16_t));
    if (pcm == NULL)
    {
        _set_status(MUSIC_PLAYBACK_ERROR, MUSIC_SERVICE_FAILURE_INTERNAL);
        failed = true;
        goto cleanup;
    }
    uint64_t total_frames = 0U;
    while (!atomic_load_explicit(&s_stream.stop, memory_order_acquire))
    {
        if (atomic_load_explicit(&s_stream.pause, memory_order_acquire))
        {
            _set_status(MUSIC_PLAYBACK_PAUSED, MUSIC_SERVICE_FAILURE_NONE);
            struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000L };
            nanosleep(&delay, NULL);
            continue;
        }
        _set_status(MUSIC_PLAYBACK_PLAYING, MUSIC_SERVICE_FAILURE_NONE);
        const sf_count_t frames = s_stream.sndfile.readf_short(
                                      sndfile, pcm,
                                      SIM_STREAM_CHUNK_FRAMES);
        if (frames <= 0)
        {
            natural_end = true;
            break;
        }
        size_t written = 0U;
        const size_t bytes = (size_t)frames * (size_t)info.channels *
                             sizeof(int16_t);
        const esp_err_t write_result = audio_service_write_leased(
                                           lease, pcm, bytes, &written, 1000U);
        if (write_result != ESP_OK || written != bytes)
        {
            _set_status(MUSIC_PLAYBACK_ERROR,
                        write_result == ESP_ERR_TIMEOUT ?
                        MUSIC_SERVICE_FAILURE_AUDIO_BUSY :
                        MUSIC_SERVICE_FAILURE_DECODE);
            failed = true;
            break;
        }
        total_frames += (uint64_t)frames;
        position_ms = (uint32_t)(total_frames * 1000U /
                                 (uint64_t)info.samplerate);
        _set_position(position_ms);
        const uint64_t sleep_ns = (uint64_t)frames * 1000000000ULL /
                                  (uint64_t)info.samplerate;
        struct timespec delay =
        {
            .tv_sec = (time_t)(sleep_ns / 1000000000ULL),
            .tv_nsec = (long)(sleep_ns % 1000000000ULL),
        };
        nanosleep(&delay, NULL);
    }
    free(pcm);
    if (atomic_load_explicit(&s_stream.stop, memory_order_acquire))
    {
        stopped = true;
    }

cleanup:
    if (audio_ready)
    {
        (void)audio_service_release(lease);
    }
    if (sndfile != NULL)
    {
        s_stream.sndfile.close(sndfile);
    }
    if (path[0] != '\0')
    {
        unlink(path);
    }
    if (stopped)
    {
        _set_status(MUSIC_PLAYBACK_IDLE, MUSIC_SERVICE_FAILURE_NONE);
    }
    else if (!failed && natural_end &&
             !atomic_load_explicit(&s_stream.shutdown, memory_order_acquire))
    {
        _set_status(MUSIC_PLAYBACK_IDLE, MUSIC_SERVICE_FAILURE_NONE);
        music_service_stream_finished();
    }
    else if (!failed)
    {
        _set_status(MUSIC_PLAYBACK_IDLE, MUSIC_SERVICE_FAILURE_NONE);
    }
    pthread_mutex_lock(&s_stream.lock);
    if (s_stream.active_generation == generation)
    {
        s_stream.thread_started = false;
    }
    pthread_mutex_unlock(&s_stream.lock);
    free(command);
    return NULL;
}

esp_err_t music_stream_init(uint32_t priority)
{
    (void)priority;
    if (s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_stream, 0, sizeof(s_stream));
    if (pthread_mutex_init(&s_stream.lock, NULL) != 0)
    {
        return ESP_ERR_NO_MEM;
    }
    s_stream.sndfile_ready = _sndfile_open(&s_stream.sndfile);
    atomic_store(&s_stream.stop, false);
    atomic_store(&s_stream.pause, false);
    atomic_store(&s_stream.shutdown, false);
    atomic_store(&s_stream.public_state, MUSIC_PLAYBACK_IDLE);
    s_stream.initialized = true;
    if (!s_stream.sndfile_ready)
    {
        LOG_W("libsndfile unavailable; CDN stream decoding disabled");
    }
    return ESP_OK;
}

esp_err_t music_stream_deinit(void)
{
    if (!s_stream.initialized)
    {
        return ESP_OK;
    }
    atomic_store_explicit(&s_stream.shutdown, true, memory_order_release);
    atomic_store_explicit(&s_stream.stop, true, memory_order_release);
    pthread_t thread;
    bool started;
    pthread_mutex_lock(&s_stream.lock);
    thread = s_stream.thread;
    started = s_stream.thread_started;
    pthread_mutex_unlock(&s_stream.lock);
    if (started && !pthread_equal(thread, pthread_self()))
    {
        pthread_join(thread, NULL);
    }
    _sndfile_close(&s_stream.sndfile);
    pthread_mutex_destroy(&s_stream.lock);
    memset(&s_stream, 0, sizeof(s_stream));
    return ESP_OK;
}

esp_err_t music_stream_play(const char *url, uint32_t duration_ms)
{
    if (!s_stream.initialized || url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_stream.sndfile_ready)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    (void)music_stream_stop();
    sim_stream_command_t *command = calloc(1, sizeof(*command));
    if (command == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    (void)snprintf(command->url, sizeof(command->url), "%s", url);
    command->duration_ms = duration_ms;
    atomic_store_explicit(&s_stream.stop, false, memory_order_release);
    atomic_store_explicit(&s_stream.pause, false, memory_order_release);
    pthread_mutex_lock(&s_stream.lock);
    command->generation = ++s_stream.active_generation;
    pthread_mutex_unlock(&s_stream.lock);
    pthread_t thread;
    if (pthread_create(&thread, NULL, _stream_task, command) != 0)
    {
        free(command);
        return ESP_ERR_NO_MEM;
    }
    pthread_mutex_lock(&s_stream.lock);
    s_stream.thread = thread;
    s_stream.thread_started = true;
    pthread_mutex_unlock(&s_stream.lock);
    return ESP_OK;
}

esp_err_t music_stream_pause(bool pause)
{
    if (!s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_stream.pause, pause, memory_order_release);
    return ESP_OK;
}

esp_err_t music_stream_stop(void)
{
    if (!s_stream.initialized)
    {
        return ESP_OK;
    }
    atomic_store_explicit(&s_stream.stop, true, memory_order_release);
    pthread_t thread;
    bool started;
    pthread_mutex_lock(&s_stream.lock);
    thread = s_stream.thread;
    started = s_stream.thread_started;
    pthread_mutex_unlock(&s_stream.lock);
    if (started && !pthread_equal(thread, pthread_self()))
    {
        pthread_join(thread, NULL);
    }
    return ESP_OK;
}

bool music_stream_is_playing(void)
{
    const int state = atomic_load_explicit(&s_stream.public_state,
                                           memory_order_acquire);
    return s_stream.initialized &&
           (state == (int)MUSIC_PLAYBACK_PLAYING ||
            state == (int)MUSIC_PLAYBACK_BUFFERING);
}

esp_err_t music_stream_get_status(music_stream_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    pthread_mutex_lock(&s_stream.lock);
    *status = s_stream.status;
    pthread_mutex_unlock(&s_stream.lock);
    return ESP_OK;
}

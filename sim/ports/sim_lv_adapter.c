/** @file esp_lv_adapter host shim: real LVGL on a pthread worker + SDL blit.
 *
 * Contract: layers/app_manager/app_core/tests/host/esp_lv_adapter.h (copied
 * to sim/stubs/include). Key device-semantics decisions:
 *  - worker task named "lvgl" owns all LVGL calls; lock/unlock is a recursive
 *    pthread mutex held across lv_timer_handler;
 *  - flush path is the ONLY pixel writer: compact stride -> custom_draw_bitmap
 *    -> esp_lcd_panel_draw_bitmap (sim_bsp blit) -> synchronous
 *    lv_display_flush_ready (no DMA ISR on the host);
 *  - no lv_draw_sw_rgb565_swap (host framebuffer is native RGB565);
 *  - tick reads CLOCK_MONOTONIC until CI step mode is enabled (only ever
 *    after mailbox s_ready), then from an explicit counter;
 *  - SDL is never called from the worker.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sim_lv_adapter.h"
#include "sim_mmap_assets.h"
#include "sim_fs.h"

#include "esp_lcd_panel_ops.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CACHE_H

static pthread_mutex_t s_lv_lock;
static pthread_once_t s_lock_once = PTHREAD_ONCE_INIT;
static TaskHandle_t s_worker_task;
static atomic_bool s_worker_stop;
static atomic_bool s_worker_running;
static atomic_bool s_paused;
static atomic_bool s_initialized;
static atomic_bool s_started;
static atomic_ullong s_frame_count;

static lv_display_t *s_display;
static lv_indev_t *s_touch_indev;
static esp_lcd_panel_handle_t s_panel;
static void *s_draw_buf_base;
static esp_lv_adapter_draw_bitmap_callbacks_t s_draw_callbacks;
static bool s_draw_callbacks_set;

/* Atomic pointer state: written by SDL main thread or Agent, read on worker. */
static _Atomic int16_t s_pointer_x;
static _Atomic int16_t s_pointer_y;
static _Atomic bool s_pointer_pressed;

/* CI step gating. */
static atomic_bool s_ci_mode;
static pthread_mutex_t s_step_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_step_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_step_done = PTHREAD_COND_INITIALIZER;
static uint64_t s_ci_base_ns;
static uint32_t s_ci_ms;
static uint32_t s_ci_target_ms;
static bool s_step_pending;
static bool s_step_ack;
static bool s_settle_request;
static bool s_settle_ack;

static uint32_t _now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      ((uint64_t)ts.tv_nsec / 1000000ULL));
}

static uint32_t _tick_cb(void)
{
    if (!atomic_load(&s_ci_mode))
    {
        return _now_ms();
    }
    return s_ci_ms;
}

static void _lock_init_once(void)
{
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&s_lv_lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
}

esp_err_t esp_lv_adapter_init(const esp_lv_adapter_config_t *config)
{
    (void)config;
    (void)pthread_once(&s_lock_once, _lock_init_once);
    if (atomic_load(&s_initialized))
    {
        return ESP_ERR_INVALID_STATE;
    }
    lv_init();
    lv_tick_set_cb(_tick_cb);
    atomic_store(&s_frame_count, 0ULL);
    atomic_store(&s_initialized, true);
    return ESP_OK;
}

bool esp_lv_adapter_is_initialized(void)
{
    return atomic_load(&s_initialized);
}

esp_err_t esp_lv_adapter_lock(int32_t timeout_ms)
{
    (void)timeout_ms;
    (void)pthread_once(&s_lock_once, _lock_init_once);
    if (pthread_mutex_lock(&s_lv_lock) != 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void esp_lv_adapter_unlock(void)
{
    (void)pthread_mutex_unlock(&s_lv_lock);
}

/* ------------------------------------------------------------------ pixels */

static void _flush_cb(lv_display_t *display, const lv_area_t *area,
                      uint8_t *px_map)
{
    const int width = lv_area_get_width(area);
    const int height = lv_area_get_height(area);
    const size_t stride = lv_draw_buf_width_to_stride((uint32_t)width,
                          lv_display_get_color_format(display));
    const size_t packed = (size_t)width * 2U;
    uint8_t *buffer = px_map;

    if (stride != packed)
    {
        uint8_t *compact = malloc(packed * (size_t)height);
        if (compact == NULL)
        {
            lv_display_flush_ready(display);
            return;
        }
        for (int y = 0; y < height; y++)
        {
            memcpy(compact + ((size_t)y * packed), px_map + ((size_t)y * stride),
                   packed);
        }
        buffer = compact;
    }

    /* No lv_draw_sw_rgb565_swap here: the host framebuffer is native
     * RGB565, while the device swap exists for the QSPI panel byte order. */
    if (s_draw_callbacks_set && (s_draw_callbacks.custom_draw_bitmap != NULL))
    {
        (void)s_draw_callbacks.custom_draw_bitmap(display, s_panel,
                area->x1, area->y1,
                area->x2 + 1, area->y2 + 1,
                buffer, NULL);
    }
    else
    {
        (void)esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                        area->x2 + 1, area->y2 + 1, buffer);
    }
    if (buffer != px_map)
    {
        free(buffer);
    }
    /* Host has no DMA completion ISR: report ready synchronously. */
    lv_display_flush_ready(display);
    atomic_fetch_add(&s_frame_count, 1ULL);
    (void)lv_display_send_event(display, LV_EVENT_REFR_READY, NULL);
}

lv_display_t *esp_lv_adapter_register_display(
    const esp_lv_adapter_display_config_t *config)
{
    const uint32_t hor = config->profile.hor_res;
    const uint32_t ver = config->profile.ver_res;
    uint32_t buffer_height = config->profile.buffer_height;
    uint32_t buffers = config->profile.require_double_buffer ? 2U : 1U;
    size_t buf_size;
    uint8_t *buf1;
    uint8_t *buf2 = NULL;
    lv_display_t *display;

    if ((buffer_height == 0U) || (buffer_height > ver))
    {
        buffer_height = ver;
    }
    buf_size = (size_t)lv_draw_buf_width_to_stride(hor, LV_COLOR_FORMAT_RGB565) *
               buffer_height;
    buf1 = malloc(buf_size * buffers);
    if (buf1 == NULL)
    {
        return NULL;
    }
    s_draw_buf_base = buf1;
    if (buffers == 2U)
    {
        buf2 = buf1 + buf_size;
    }
    display = lv_display_create((int32_t)hor, (int32_t)ver);
    if (display == NULL)
    {
        free(buf1);
        return NULL;
    }
    lv_display_set_buffers(display, buf1, buf2, (uint32_t)buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, _flush_cb);
    s_display = display;
    s_panel = config->panel;
    return display;
}

lv_display_t *sim_lv_display(void)
{
    return s_display;
}

esp_err_t esp_lv_adapter_set_draw_bitmap_callbacks(
    lv_display_t *display,
    const esp_lv_adapter_draw_bitmap_callbacks_t *callbacks,
    void *user_ctx)
{
    (void)display;
    (void)user_ctx;
    if (callbacks == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_draw_callbacks = *callbacks;
    s_draw_callbacks_set = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------- touch */

void sim_lv_touch_update(int16_t x, int16_t y, bool pressed)
{
    atomic_store(&s_pointer_x, x);
    atomic_store(&s_pointer_y, y);
    atomic_store(&s_pointer_pressed, pressed);
}

static void _indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = atomic_load(&s_pointer_x);
    data->point.y = atomic_load(&s_pointer_y);
    data->state = atomic_load(&s_pointer_pressed)
                  ? LV_INDEV_STATE_PRESSED
                  : LV_INDEV_STATE_RELEASED;
}

lv_indev_t *esp_lv_adapter_register_touch(
    const esp_lv_adapter_touch_config_t *config)
{
    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL)
    {
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, _indev_read_cb);
    lv_indev_set_display(indev, config->disp != NULL
                         ? config->disp
                         : s_display);
    s_touch_indev = indev;
    return indev;
}

bool esp_lv_adapter_touch_notify_interrupt(lv_indev_t *touch)
{
    (void)touch;
    return true;
}

/* ------------------------------------------------------------------- fonts */

typedef struct sim_font_handle
{
    lv_font_t *font;
} sim_font_handle_t;

esp_err_t esp_lv_adapter_fs_mount(const fs_cfg_t *config,
                                  esp_lv_fs_handle_t *ret_handle)
{
    const esp_err_t result = sim_fs_mount(config->fs_letter,
                                          config->fs_assets);
    if ((result == ESP_OK) && (ret_handle != NULL))
    {
        *ret_handle = (esp_lv_fs_handle_t)(intptr_t)config->fs_letter;
    }
    return result;
}

esp_err_t esp_lv_adapter_ft_font_init(
    const esp_lv_adapter_ft_font_config_t *config,
    esp_lv_adapter_ft_font_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL) || (config->name == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    sim_font_handle_t *created = calloc(1, sizeof(*created));
    if (created == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    created->font = lv_freetype_font_create(config->name,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                            config->size,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    if (created->font == NULL)
    {
        fprintf(stderr, "sim_adapter: font create failed: %s size %u\n",
                config->name, (unsigned)config->size);
        free(created);
        return ESP_FAIL;
    }
    *handle = created;
    return ESP_OK;
}

const lv_font_t *esp_lv_adapter_ft_font_get(
    esp_lv_adapter_ft_font_handle_t handle)
{
    const sim_font_handle_t *font = handle;
    return (font != NULL) ? font->font : NULL;
}

esp_err_t esp_lv_adapter_ft_font_deinit(
    esp_lv_adapter_ft_font_handle_t handle)
{
    sim_font_handle_t *font = handle;
    if (font == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    lv_freetype_font_delete(font->font);
    free(font);
    return ESP_OK;
}

/* ----------------------------------------------------------------- worker */

static uint32_t _drain_ready_timers(void)
{
    uint32_t result;
    do
    {
        result = lv_timer_handler();
    }
    while (result == 0U);
    return result;
}

static void _worker_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_worker_stop))
    {
        if (atomic_load(&s_ci_mode))
        {
            (void)pthread_mutex_lock(&s_step_lock);
            while (!s_step_pending && !s_settle_request &&
                    !atomic_load(&s_worker_stop))
            {
                (void)pthread_cond_wait(&s_step_wake, &s_step_lock);
            }
            if (s_step_pending)
            {
                s_ci_ms = (uint32_t)((s_ci_target_ms > s_ci_ms)
                                     ? s_ci_target_ms
                                     : s_ci_ms);
            }
            (void)pthread_mutex_unlock(&s_step_lock);
            if (atomic_load(&s_worker_stop))
            {
                break;
            }
            if (!atomic_load(&s_paused))
            {
                (void)pthread_mutex_lock(&s_lv_lock);
                if (!atomic_load(&s_paused))
                {
                    _drain_ready_timers();
                }
                (void)pthread_mutex_unlock(&s_lv_lock);
            }
            (void)pthread_mutex_lock(&s_step_lock);
            s_step_pending = false;
            s_settle_request = false;
            s_step_ack = true;
            s_settle_ack = true;
            (void)pthread_cond_broadcast(&s_step_done);
            (void)pthread_mutex_unlock(&s_step_lock);
            continue;
        }
        if (!atomic_load(&s_paused))
        {
            (void)pthread_mutex_lock(&s_lv_lock);
            if (!atomic_load(&s_paused))
            {
                (void)lv_timer_handler();
            }
            (void)pthread_mutex_unlock(&s_lv_lock);
        }
        usleep(5000);
    }
    atomic_store(&s_worker_running, false);
}

esp_err_t esp_lv_adapter_start(void)
{
    if (atomic_load(&s_started))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_load(&s_initialized))
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_worker_stop, false);
    atomic_store(&s_paused, false);
    if (xTaskCreatePinnedToCore(_worker_main, "lvgl",
                                CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE,
                                NULL, 5U, &s_worker_task,
                                CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID)
            != pdPASS)
    {
        return ESP_FAIL;
    }
    atomic_store(&s_worker_running, true);
    atomic_store(&s_started, true);
    return ESP_OK;
}

esp_err_t esp_lv_adapter_pause(int32_t timeout_ms)
{
    const esp_err_t result = esp_lv_adapter_lock(timeout_ms);
    if (result != ESP_OK)
    {
        return result;
    }
    fprintf(stderr, "sim_adapter: pause\n");
    atomic_store(&s_paused, true);
    esp_lv_adapter_unlock();
    return ESP_OK;
}

esp_err_t esp_lv_adapter_resume(void)
{
    const esp_err_t result = esp_lv_adapter_lock(-1);
    if (result != ESP_OK)
    {
        return result;
    }
    fprintf(stderr, "sim_adapter: resume\n");
    atomic_store(&s_paused, false);
    esp_lv_adapter_unlock();
    return ESP_OK;
}

bool sim_lv_paused(void)
{
    return atomic_load(&s_paused);
}

void sim_lv_ci_mode_set(bool enabled)
{
    (void)pthread_mutex_lock(&s_step_lock);
    if (enabled && !atomic_load(&s_ci_mode))
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        s_ci_base_ns = (uint64_t)now.tv_sec * 1000000000ULL +
                       (uint64_t)now.tv_nsec;
        s_ci_ms = _now_ms();
        s_ci_target_ms = s_ci_ms;
    }
    atomic_store(&s_ci_mode, enabled);
    (void)pthread_cond_broadcast(&s_step_wake);
    (void)pthread_mutex_unlock(&s_step_lock);
}

int sim_lv_ci_step(uint32_t ms)
{
    if (!atomic_load(&s_ci_mode))
    {
        return -1;
    }
    (void)pthread_mutex_lock(&s_step_lock);
    s_ci_target_ms += ms;
    s_step_pending = true;
    s_step_ack = false;
    (void)pthread_cond_broadcast(&s_step_wake);
    while (!s_step_ack && !atomic_load(&s_worker_stop))
    {
        (void)pthread_cond_wait(&s_step_done, &s_step_lock);
    }
    (void)pthread_mutex_unlock(&s_step_lock);
    return s_step_ack ? 0 : -1;
}

int sim_lv_ci_settle(void)
{
    if (!atomic_load(&s_ci_mode))
    {
        return -1;
    }
    (void)pthread_mutex_lock(&s_step_lock);
    s_settle_request = true;
    s_settle_ack = false;
    (void)pthread_cond_broadcast(&s_step_wake);
    while (!s_settle_ack && !atomic_load(&s_worker_stop))
    {
        (void)pthread_cond_wait(&s_step_done, &s_step_lock);
    }
    (void)pthread_mutex_unlock(&s_step_lock);
    return s_settle_ack ? 0 : -1;
}

bool sim_lv_ci_enabled(void)
{
    return atomic_load(&s_ci_mode);
}

uint64_t sim_lv_frame_count(void)
{
    return atomic_load(&s_frame_count);
}

esp_err_t esp_lv_adapter_deinit(void)
{
    if (!atomic_load(&s_initialized))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_started))
    {
        atomic_store(&s_worker_stop, true);
        (void)pthread_cond_broadcast(&s_step_wake);
        /* The entry returns through the task trampoline, which self-deletes
         * and frees the task record; do not delete it here. */
        while (atomic_load(&s_worker_running))
        {
            usleep(2000U);
        }
        s_worker_task = NULL;
        atomic_store(&s_started, false);
    }
    (void)esp_lv_adapter_lock(-1);
    if (s_touch_indev != NULL)
    {
        lv_indev_delete(s_touch_indev);
        s_touch_indev = NULL;
    }
    if (s_display != NULL)
    {
        /* Draw buffers were allocated by register_display and are owned by
         * the adapter lifetime; lv_display_delete frees the display object. */
        lv_display_delete(s_display);
        s_display = NULL;
        free(s_draw_buf_base);
        s_draw_buf_base = NULL;
    }
    (void)esp_lv_adapter_unlock();
    sim_fs_unmount();
    lv_deinit();
    atomic_store(&s_initialized, false);
    return ESP_OK;
}

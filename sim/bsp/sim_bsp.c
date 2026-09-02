/** @file Simulated board implementation (M1: framebuffer + panel blit). */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "sim_bsp.h"

struct sim_panel
{
    esp_lcd_panel_t base;
};

static struct sim_panel s_panel;
static uint16_t *s_fb;
static uint32_t s_blit_count;
static esp_lcd_touch_t s_touch;
static unsigned char s_panel_io_token;
static bsp_display_port_t s_display_port;
static atomic_bool s_screen_suspended;
static atomic_bool s_screen_suspend_committed;
static atomic_uint s_brightness;
static atomic_uint s_brightness_temp;

static esp_err_t _panel_draw_bitmap(esp_lcd_panel_t *panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data);

esp_err_t sim_bsp_init(void)
{
    if (s_fb != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_fb = calloc(SIM_BSP_H_RES * SIM_BSP_V_RES, sizeof(*s_fb));
    if (s_fb == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_panel.base.draw_bitmap = _panel_draw_bitmap;
    s_blit_count = 0U;
    memset(&s_touch, 0, sizeof(s_touch));
    s_touch.config.int_gpio_num = GPIO_NUM_NC;
    memset(&s_display_port, 0, sizeof(s_display_port));
    s_display_port.width = SIM_BSP_H_RES;
    s_display_port.height = SIM_BSP_V_RES;
    s_display_port.panel = &s_panel.base;
    s_display_port.panel_io = (void *)&s_panel_io_token;
    s_display_port.touch = &s_touch;
    s_display_port.touch_io = NULL;
    s_display_port.transport.kind = BSP_DISPLAY_TRANSPORT_QSPI;
    s_display_port.transport.clock_hz = 40000000U;
    s_display_port.transport.max_transfer_lines = SIM_BSP_V_RES;
    s_display_port.transport.dma_max_full_lines = SIM_BSP_V_RES;
    s_display_port.transport.transaction_queue_depth = 2U;
    s_display_port.transport.data_lines = 4U;
    s_display_port.transport.bits_per_pixel = 16U;
    s_display_port.transport.psram_dma_direct = false;
    s_display_port.te.enabled = false;
    s_display_port.te.gpio_num = GPIO_NUM_NC;
    s_display_port.te.intr_type = 0;
    atomic_store(&s_screen_suspended, false);
    atomic_store(&s_screen_suspend_committed, false);
    atomic_store(&s_brightness, 192U);
    atomic_store(&s_brightness_temp, 192U);
    return ESP_OK;
}

void sim_bsp_deinit(void)
{
    free(s_fb);
    s_fb = NULL;
    s_blit_count = 0U;
}

const uint16_t *sim_bsp_framebuffer(void)
{
    return s_fb;
}

esp_lcd_panel_handle_t sim_bsp_panel(void)
{
    return &s_panel.base;
}

void *sim_bsp_panel_io(void)
{
    return NULL;
}

uint32_t sim_bsp_blit_count(void)
{
    return s_blit_count;
}

void sim_bsp_reset_stats(void)
{
    s_blit_count = 0U;
}

static esp_err_t _panel_draw_bitmap(esp_lcd_panel_t *panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data)
{
    int y;
    int width;

    if ((panel != &s_panel.base) || (color_data == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if ((x_start < 0) || (y_start < 0) || (x_end > (int)SIM_BSP_H_RES) ||
            (y_end > (int)SIM_BSP_V_RES) || (x_end <= x_start) ||
            (y_end <= y_start))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    width = x_end - x_start;
    for (y = y_start; y < y_end; y++)
    {
        uint16_t *dst = s_fb + (y * SIM_BSP_H_RES) + x_start;
        const uint16_t *src = (const uint16_t *)color_data;
        src += (y - y_start) * width;
        memcpy(dst, src, (size_t)width * sizeof(*dst));
    }
    s_blit_count++;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data)
{
    if ((panel == NULL) || (panel->draw_bitmap == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return panel->draw_bitmap(panel, x_start, y_start, x_end, y_end,
                              color_data);
}

esp_err_t esp_lcd_panel_io_tx_param(void *io, int lcd_cmd,
                                    const void *param, size_t param_size)
{
    (void)io;
    (void)lcd_cmd;
    (void)param;
    (void)param_size;
    return ESP_OK;
}

/* ---- display port / touch / screen ops / input bridge (M2) ---- */

#include <stdatomic.h>

#include "esp_lcd_panel_io.h"


static bool _screen_is_available(void)
{
    return true;
}

static esp_err_t _screen_suspend(void)
{
    atomic_store(&s_screen_suspended, true);
    atomic_store(&s_screen_suspend_committed, true);
    return ESP_OK;
}

static esp_err_t _screen_resume_prepare(void)
{
    atomic_store(&s_screen_suspend_committed, false);
    return ESP_OK;
}

static esp_err_t _screen_resume_commit(void)
{
    atomic_store(&s_screen_suspended, false);
    atomic_store(&s_screen_suspend_committed, false);
    atomic_store(&s_brightness, 192U);
    atomic_store(&s_brightness_temp, 192U);
    return ESP_OK;
}

static bool _screen_is_suspended(void)
{
    return atomic_load(&s_screen_suspended);
}

static bool _screen_is_suspend_committed(void)
{
    return atomic_load(&s_screen_suspend_committed);
}

static esp_err_t _screen_set_brightness(uint8_t brightness)
{
    atomic_store(&s_brightness, brightness);
    atomic_store(&s_brightness_temp, brightness);
    return ESP_OK;
}

static esp_err_t _screen_set_brightness_temp(uint8_t brightness)
{
    atomic_store(&s_brightness_temp, brightness);
    return ESP_OK;
}

static uint8_t _screen_get_brightness(void)
{
    return (uint8_t)atomic_load(&s_brightness);
}

static esp_err_t _screen_set_enabled(bool on)
{
    (void)on;
    return ESP_OK;
}

static esp_err_t _screen_set_power(bool on)
{
    (void)on;
    return ESP_OK;
}

static const bsp_screen_ops_t s_screen_ops =
{
    .is_available = _screen_is_available,
    .suspend = _screen_suspend,
    .resume_prepare = _screen_resume_prepare,
    .resume_commit = _screen_resume_commit,
    .is_suspended = _screen_is_suspended,
    .is_suspend_committed = _screen_is_suspend_committed,
    .set_brightness = _screen_set_brightness,
    .set_brightness_temp = _screen_set_brightness_temp,
    .get_brightness = _screen_get_brightness,
    .set_enabled = _screen_set_enabled,
    .set_power = _screen_set_power,
};

const bsp_screen_ops_t *sim_bsp_screen_ops(void)
{
    return &s_screen_ops;
}

bool sim_bsp_screen_is_suspended(void)
{
    return atomic_load(&s_screen_suspended);
}

uint8_t sim_bsp_brightness(void)
{
    return (uint8_t)atomic_load(&s_brightness);
}

static void (*s_input_cb)(app_manager_key_t key, app_manager_key_event_t event,
                          void *user_data);
static void *s_input_user;

esp_err_t sim_bsp_input_register(void (*cb)(app_manager_key_t key,
                                 app_manager_key_event_t event,
                                 void *user_data),
                                 void *user_data)
{
    if (cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_input_cb = cb;
    s_input_user = user_data;
    return ESP_OK;
}

esp_err_t sim_bsp_key(sim_key_t key, sim_key_action_t action)
{
    app_manager_key_t mapped;
    app_manager_key_event_t event;

    if (s_input_cb == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    mapped = (key == SIM_KEY_BOOT) ? APP_MANAGER_KEY_HOME : APP_MANAGER_KEY_POWER;
    /* Board debouncer semantics: a short press releases as UP plus CLICK. */
    event = (action == SIM_KEY_ACTION_PRESS)
            ? APP_MANAGER_KEY_EVENT_DOWN
            : APP_MANAGER_KEY_EVENT_UP;
    s_input_cb(mapped, event, s_input_user);
    if (action == SIM_KEY_ACTION_RELEASE)
    {
        s_input_cb(mapped, APP_MANAGER_KEY_EVENT_CLICK, s_input_user);
    }
    return ESP_OK;
}

const bsp_display_port_t *sim_bsp_display_port(void)
{
    return &s_display_port;
}

bsp_capabilities_t sim_bsp_capabilities(void)
{
    return (bsp_capabilities_t)(BSP_CAPABILITY_DISPLAY |
                                BSP_CAPABILITY_TOUCH |
                                BSP_CAPABILITY_INPUT);
}

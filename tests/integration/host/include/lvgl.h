/** @file Minimal LVGL API model used by integration host tests. */
#ifndef __CROSS_LAYER_LVGL_H__
#define __CROSS_LAYER_LVGL_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __CROSS_LAYER_LVGL_H__ */

/** @brief Opaque fake LVGL object. */
typedef struct lv_obj_t lv_obj_t;
/** @brief Opaque fake LVGL timer. */
typedef struct lv_timer_t lv_timer_t;
/** @brief Opaque fake LVGL event. */
typedef struct lv_event_t lv_event_t;
/** @brief Opaque fake LVGL event descriptor. */
typedef struct lv_event_dsc_t lv_event_dsc_t;
/** @brief Opaque fake LVGL display. */
typedef struct lv_display_t lv_display_t;
/** @brief Opaque fake LVGL input device. */
typedef struct lv_indev_t lv_indev_t;
/** @brief Fake LVGL animation with the public timing fields used by LVGL. */
typedef struct _lv_anim_t lv_anim_t;

/** @brief Fake LVGL color formats used by static Canvas draw buffers. */
typedef enum
{
    LV_COLOR_FORMAT_UNKNOWN = 0,
    LV_COLOR_FORMAT_A8,
} lv_color_format_t;

/** @brief Minimal fake LVGL image header carried by a draw buffer. */
typedef struct lv_image_header_t
{
    lv_color_format_t cf;
    uint32_t w;
    uint32_t h;
    uint32_t stride;
} lv_image_header_t;

/** @brief Minimal fake LVGL draw buffer used by Canvas. */
typedef struct lv_draw_buf_t
{
    lv_image_header_t header;
    uint32_t data_size;
    uint8_t *data;
    void *unaligned_data;
    uint32_t flush_count;
} lv_draw_buf_t;

/** @brief Logical point reported by the fake pointer input device. */
typedef struct lv_point_t
{
    int32_t x;
    int32_t y;
} lv_point_t;

/** @brief Fake pointer input state. */
typedef enum
{
    LV_INDEV_STATE_RELEASED = 0,
    LV_INDEV_STATE_PRESSED,
} lv_indev_state_t;

/** @brief Fake LVGL font object. */
typedef struct lv_font_t
{
    uint8_t marker;
} lv_font_t;

typedef uint32_t lv_color_t;
typedef int32_t lv_event_code_t;
typedef uint8_t lv_opa_t;
typedef uint32_t lv_obj_flag_t;
typedef uint32_t lv_part_t;
typedef uint32_t lv_style_selector_t;
typedef uint32_t lv_state_t;
/** @brief Fake LVGL event callback. */
typedef void (*lv_event_cb_t)(lv_event_t *event);
/** @brief Fake LVGL timer callback. */
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);
/** @brief Fake LVGL animation value callback. */
typedef void (*lv_anim_exec_xcb_t)(void *variable, int32_t value);

/** @brief Minimal public LVGL animation descriptor used for fast-forwarding. */
struct _lv_anim_t
{
    void *var;
    lv_anim_exec_xcb_t exec_cb;
    int32_t start_value;
    int32_t current_value;
    int32_t end_value;
    int32_t duration;
    int32_t act_time;
};

/** @brief LVGL 9.5 screen-load effects. */
typedef enum
{
    LV_SCREEN_LOAD_ANIM_NONE = 0,
    LV_SCREEN_LOAD_ANIM_OVER_LEFT,
    LV_SCREEN_LOAD_ANIM_OVER_RIGHT,
    LV_SCREEN_LOAD_ANIM_OVER_TOP,
    LV_SCREEN_LOAD_ANIM_OVER_BOTTOM,
    LV_SCREEN_LOAD_ANIM_MOVE_LEFT,
    LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
    LV_SCREEN_LOAD_ANIM_MOVE_TOP,
    LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM,
    LV_SCREEN_LOAD_ANIM_FADE_IN,
    LV_SCREEN_LOAD_ANIM_FADE_ON = LV_SCREEN_LOAD_ANIM_FADE_IN,
    LV_SCREEN_LOAD_ANIM_FADE_OUT,
    LV_SCREEN_LOAD_ANIM_OUT_LEFT,
    LV_SCREEN_LOAD_ANIM_OUT_RIGHT,
    LV_SCREEN_LOAD_ANIM_OUT_TOP,
    LV_SCREEN_LOAD_ANIM_OUT_BOTTOM,
} lv_screen_load_anim_t;

extern const lv_font_t host_lv_default_font;

#define LV_FONT_DEFAULT (&host_lv_default_font)

#define LV_USE_CANVAS                    1
#define LV_DRAW_BUF_STRIDE(width, format) ((uint32_t)(width))
#define LV_DRAW_BUF_SIZE(width, height, format) \
    (LV_DRAW_BUF_STRIDE((width), (format)) * (uint32_t)(height))
#define LV_DRAW_BUF_DEFINE_STATIC(name, width, height, format) \
    static uint8_t buf_##name[LV_DRAW_BUF_SIZE((width), (height), (format))]; \
    static lv_draw_buf_t name = \
    { \
        .header = \
        { \
            .cf = (format), \
            .w = (width), \
            .h = (height), \
            .stride = LV_DRAW_BUF_STRIDE((width), (format)), \
        }, \
        .data_size = sizeof(buf_##name), \
        .data = buf_##name, \
        .unaligned_data = buf_##name, \
    }
#define LV_DRAW_BUF_INIT_STATIC(name) \
    do \
    { \
        (name).data = buf_##name; \
        (name).unaligned_data = buf_##name; \
        (name).flush_count = 0U; \
    } while (0)

#define LV_ALIGN_TOP_LEFT             0
#define LV_ALIGN_CENTER               1
#define LV_ANIM_OFF                   0
#define LV_ANIM_ON                    1
#define LV_DIR_VER                    1
#define LV_EVENT_ALL                  0
#define LV_EVENT_CLICKED              1
#define LV_EVENT_VALUE_CHANGED        2
#define LV_EVENT_RELEASED             3
#define LV_EVENT_SCREEN_UNLOAD_START  4
#define LV_EVENT_SCREEN_LOAD_START    5
#define LV_EVENT_SCREEN_LOADED        6
#define LV_EVENT_SCREEN_UNLOADED      7
#define LV_EVENT_PRESSED              8
#define LV_EVENT_PRESSING             9
#define LV_EVENT_PRESS_LOST           10
#define LV_EVENT_INDEV_RESET          11
#define LV_EVENT_CANCEL               12
#define LV_EVENT_DELETE               13
#define LV_EVENT_SHORT_CLICKED        14
#define LV_EVENT_LONG_PRESSED         15
#define LV_EVENT_LONG_PRESSED_REPEAT  16
#define LV_EVENT_GESTURE              17
#define LV_FLEX_ALIGN_START           0
#define LV_FLEX_ALIGN_CENTER          1
#define LV_FLEX_ALIGN_SPACE_BETWEEN   2
#define LV_FLEX_FLOW_COLUMN           0
#define LV_FLEX_FLOW_ROW              1
#define LV_LABEL_LONG_DOT             0
#define LV_LABEL_LONG_WRAP            1
#define LV_OBJ_FLAG_HIDDEN            (1U << 0)
#define LV_OBJ_FLAG_CLICKABLE         (1U << 1)
#define LV_OBJ_FLAG_CLICK_FOCUSABLE   (1U << 2)
#define LV_OBJ_FLAG_SCROLLABLE        (1U << 4)
#define LV_OBJ_FLAG_SCROLL_ELASTIC    (1U << 5)
#define LV_OBJ_FLAG_SCROLL_MOMENTUM   (1U << 6)
#define LV_OBJ_FLAG_SCROLL_CHAIN_HOR  (1U << 8)
#define LV_OBJ_FLAG_SCROLL_CHAIN_VER  (1U << 9)
#define LV_OBJ_FLAG_SCROLL_CHAIN      \
    (LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER)
#define LV_OBJ_FLAG_SCROLL_WITH_ARROW (1U << 11)
#define LV_OBJ_FLAG_SNAPPABLE         (1U << 12)
#define LV_OBJ_FLAG_PRESS_LOCK        (1U << 13)
#define LV_OBJ_FLAG_EVENT_BUBBLE      (1U << 14)
#define LV_OBJ_FLAG_GESTURE_BUBBLE    (1U << 15)
#define LV_OBJ_FLAG_IGNORE_LAYOUT     (1U << 17)
#define LV_OBJ_FLAG_FLOATING          (1U << 18)
#define LV_OPA_TRANSP                 0
#define LV_OPA_COVER                  255
#define LV_PART_MAIN                  0
#define LV_PART_INDICATOR             1
#define LV_PART_KNOB                  2
#define LV_SCROLLBAR_MODE_AUTO        0
#define LV_SIZE_CONTENT               (-1)
#define LV_RADIUS_CIRCLE              32767
#define LV_STATE_PRESSED              1
#define LV_STATE_DISABLED             (1U << 7)
#define LV_TEXT_ALIGN_CENTER          0
#define LV_TEXT_ALIGN_RIGHT           1

#define HOST_LV_PCT_BASE              INT32_C(0x40000000)
#define LV_PCT(value)                 (HOST_LV_PCT_BASE + (value))

#define LV_SYMBOL_EYE_OPEN  "eye"
#define LV_SYMBOL_AUDIO     "audio"
#define LV_SYMBOL_BELL      "bell"
#define LV_SYMBOL_BACKSPACE "backspace"
#define LV_SYMBOL_CLOSE     "close"
#define LV_SYMBOL_GPS       "gps"
#define LV_SYMBOL_HOME      "home"
#define LV_SYMBOL_LEFT      "left"
#define LV_SYMBOL_LIST      "list"
#define LV_SYMBOL_MUTE      "mute"
#define LV_SYMBOL_OK        "ok"
#define LV_SYMBOL_PLAY      "play"
#define LV_SYMBOL_POWER     "power"
#define LV_SYMBOL_REFRESH   "refresh"
#define LV_SYMBOL_RIGHT     "right"
#define LV_SYMBOL_SAVE      "save"
#define LV_SYMBOL_SD_CARD   "sd-card"
#define LV_SYMBOL_SETTINGS  "settings"
#define LV_SYMBOL_STOP      "stop"
#define LV_SYMBOL_VOLUME_MAX "volume-max"
#define LV_SYMBOL_WARNING   "warning"
#define LV_SYMBOL_WIFI      "wifi"

/** @brief Return the fake active screen. */
lv_obj_t *lv_screen_active(void);
/** @brief Return the singleton fake display. */
lv_display_t *lv_display_get_default(void);
/** @brief Return the active screen for the fake display. */
lv_obj_t *lv_display_get_screen_active(lv_display_t *display);
/** @brief Return the top layer for the fake display. */
lv_obj_t *lv_display_get_layer_top(lv_display_t *display);
/** @brief Return the system layer for the fake display. */
lv_obj_t *lv_display_get_layer_sys(lv_display_t *display);
/** @brief Return the fake display's logical horizontal resolution. */
int32_t lv_display_get_horizontal_resolution(const lv_display_t *display);
/** @brief Return the fake display's logical vertical resolution. */
int32_t lv_display_get_vertical_resolution(const lv_display_t *display);
/** @brief Load a fake screen immediately. */
void lv_screen_load(lv_obj_t *screen);
/** @brief Load a fake screen with an LVGL 9.5 screen transition. */
void lv_screen_load_anim(lv_obj_t *screen, lv_screen_load_anim_t animation,
                         uint32_t time, uint32_t delay, bool auto_delete);
/** @brief Return the persistent fake top layer. */
lv_obj_t *lv_layer_top(void);
/** @brief Return the persistent fake system layer. */
lv_obj_t *lv_layer_sys(void);
/** @brief Create a fake generic object. */
lv_obj_t *lv_obj_create(lv_obj_t *parent);
/** @brief Create a fake button. */
lv_obj_t *lv_button_create(lv_obj_t *parent);
/** @brief Create a fake label. */
lv_obj_t *lv_label_create(lv_obj_t *parent);
/** @brief Create a fake slider. */
lv_obj_t *lv_slider_create(lv_obj_t *parent);
/** @brief Create a fake progress bar. */
lv_obj_t *lv_bar_create(lv_obj_t *parent);
/** @brief Create a fake Canvas. */
lv_obj_t *lv_canvas_create(lv_obj_t *parent);
/** @brief Bind a static fake draw buffer to a Canvas. */
void lv_canvas_set_draw_buf(lv_obj_t *canvas, lv_draw_buf_t *draw_buf);
/** @brief Flush a fake draw buffer after direct pixel writes. */
void lv_draw_buf_flush_cache(lv_draw_buf_t *draw_buf, const void *area);
/** @brief Delete a fake object. */
void lv_obj_delete(lv_obj_t *object);
/** @brief Delete all children from a fake object. */
void lv_obj_clean(lv_obj_t *object);
/** @brief Register a fake event callback. */
lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *object,
                                    lv_event_cb_t callback,
                                    lv_event_code_t code, void *user_data);
/** @brief Remove one fake object event descriptor. */
bool lv_obj_remove_event_dsc(lv_obj_t *object, lv_event_dsc_t *descriptor);
/** @brief Add behavior flags to a fake object. */
void lv_obj_add_flag(lv_obj_t *object, lv_obj_flag_t flags);
/** @brief Remove behavior flags from a fake object. */
void lv_obj_remove_flag(lv_obj_t *object, lv_obj_flag_t flags);
/** @brief Return whether all requested fake object flags are set. */
bool lv_obj_has_flag(const lv_obj_t *object, lv_obj_flag_t flags);
/** @brief Add state flags to a fake object. */
void lv_obj_add_state(lv_obj_t *object, lv_state_t state);
/** @brief Remove state flags from a fake object. */
void lv_obj_remove_state(lv_obj_t *object, lv_state_t state);
/** @brief Return whether all requested fake object states are set. */
bool lv_obj_has_state(const lv_obj_t *object, lv_state_t state);
/** @brief Return whether a fake object remains valid. */
bool lv_obj_is_valid(const lv_obj_t *object);
/** @brief Return a fake object's parent. */
lv_obj_t *lv_obj_get_parent(const lv_obj_t *object);
/** @brief Return a fake object's display. */
lv_display_t *lv_obj_get_display(const lv_obj_t *object);
/** @brief Move an object within its fake parent stacking order. */
void lv_obj_move_to_index(lv_obj_t *object, int32_t index);
/** @brief Set a fake object's position. */
void lv_obj_set_pos(lv_obj_t *object, int32_t x, int32_t y);
/** @brief Set a fake object's horizontal position. */
void lv_obj_set_x(lv_obj_t *object, int32_t x);
/** @brief Set a fake object's vertical position. */
void lv_obj_set_y(lv_obj_t *object, int32_t y);
/** @brief Return a fake object's horizontal position. */
int32_t lv_obj_get_x(const lv_obj_t *object);
/** @brief Return a fake object's vertical position. */
int32_t lv_obj_get_y(const lv_obj_t *object);
/** @brief Set a fake object's dimensions. */
void lv_obj_set_size(lv_obj_t *object, int32_t width, int32_t height);
/** @brief Set a fake object's width. */
void lv_obj_set_width(lv_obj_t *object, int32_t width);
/** @brief Set a fake object's height. */
void lv_obj_set_height(lv_obj_t *object, int32_t height);
/** @brief Return a fake object's width. */
int32_t lv_obj_get_width(const lv_obj_t *object);
/** @brief Return a fake object's height. */
int32_t lv_obj_get_height(const lv_obj_t *object);
/** @brief Set a fake object's inherited opacity. */
void lv_obj_set_style_opa(lv_obj_t *object, lv_opa_t opacity,
                          lv_style_selector_t selector);
/** @brief Return a fake object's inherited opacity. */
lv_opa_t lv_obj_get_style_opa(const lv_obj_t *object, lv_part_t part);
/** @brief Set a fake object's background opacity. */
void lv_obj_set_style_bg_opa(lv_obj_t *object, lv_opa_t opacity,
                             lv_style_selector_t selector);
/** @brief Return a fake object's background opacity. */
lv_opa_t lv_obj_get_style_bg_opa(const lv_obj_t *object, lv_part_t part);
/** @brief Set a fake object's text opacity. */
void lv_obj_set_style_text_opa(lv_obj_t *object, lv_opa_t opacity,
                               lv_style_selector_t selector);
/** @brief Set the recolor used to render an A8 Canvas. */
void lv_obj_set_style_image_recolor(lv_obj_t *object, lv_color_t color,
                                    lv_style_selector_t selector);
/** @brief Set the fixed image opacity used to render a Canvas. */
void lv_obj_set_style_image_opa(lv_obj_t *object, lv_opa_t opacity,
                                lv_style_selector_t selector);
/** @brief Remove all fake object styles. */
void lv_obj_remove_style_all(lv_obj_t *object);
/** @brief Record invalidation of a fake object. */
void lv_obj_invalidate(lv_obj_t *object);

/** @brief Return a fake event code. */
lv_event_code_t lv_event_get_code(lv_event_t *event);
/** @brief Return fake event user data. */
void *lv_event_get_user_data(lv_event_t *event);
/** @brief Return the fake event target. */
lv_obj_t *lv_event_get_target(lv_event_t *event);
/** @brief Return the fake event target as an object. */
lv_obj_t *lv_event_get_target_obj(lv_event_t *event);
/** @brief Return the fake event's current target as an object. */
lv_obj_t *lv_event_get_current_target_obj(lv_event_t *event);
/** @brief Return the input device which generated a pointer event. */
lv_indev_t *lv_event_get_indev(lv_event_t *event);
/** @brief Return the fake event parameter. */
void *lv_event_get_param(lv_event_t *event);

/** @brief Return the input device currently dispatching an event. */
lv_indev_t *lv_indev_active(void);
/** @brief Return the fake pointer's current logical point. */
void lv_indev_get_point(const lv_indev_t *indev, lv_point_t *point);
/** @brief Return the fake pointer's current state. */
lv_indev_state_t lv_indev_get_state(const lv_indev_t *indev);
/** @brief Reset the fake input device and its active object. */
void lv_indev_reset(lv_indev_t *indev, lv_obj_t *object);
/** @brief Consume input events until the pointer is released. */
void lv_indev_wait_release(lv_indev_t *indev);

/** @brief Set fake label text. */
void lv_label_set_text(lv_obj_t *label, const char *text);
/** @brief Set formatted fake label text. */
void lv_label_set_text_fmt(lv_obj_t *label, const char *format, ...);
/** @brief Set fake progress-bar range. */
void lv_bar_set_range(lv_obj_t *bar, int32_t minimum, int32_t maximum);
/** @brief Set fake progress-bar value. */
void lv_bar_set_value(lv_obj_t *bar, int32_t value, int animation);
/** @brief Set fake slider range. */
void lv_slider_set_range(lv_obj_t *slider, int32_t minimum, int32_t maximum);
/** @brief Set fake slider value. */
void lv_slider_set_value(lv_obj_t *slider, int32_t value, int animation);
/** @brief Return fake slider value. */
int32_t lv_slider_get_value(lv_obj_t *slider);

/** @brief Create a periodic fake LVGL timer. */
lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period,
                            void *user_data);
/** @brief Delete a fake LVGL timer. */
void lv_timer_delete(lv_timer_t *timer);
/** @brief Pause a fake LVGL timer. */
void lv_timer_pause(lv_timer_t *timer);
/** @brief Resume a fake LVGL timer. */
void lv_timer_resume(lv_timer_t *timer);
/** @brief Mark a fake LVGL timer ready to run. */
void lv_timer_ready(lv_timer_t *timer);
/** @brief Return fake timer user data. */
void *lv_timer_get_user_data(lv_timer_t *timer);

/** @brief Find a running fake animation for a variable. */
lv_anim_t *lv_anim_get(void *variable, lv_anim_exec_xcb_t execute_callback);
/** @brief Delete matching fake animations. */
bool lv_anim_delete(void *variable, lv_anim_exec_xcb_t execute_callback);
/** @brief Refresh and complete fake animations whose active time elapsed. */
void lv_anim_refr_now(void);

/** @brief Convert a host RGB value to the fake LVGL color type. */
static inline lv_color_t lv_color_hex(uint32_t color)
{
    return color;
}

/** @brief Return the fake LVGL black color. */
static inline lv_color_t lv_color_black(void)
{
    return 0U;
}

/** @brief Return the fake LVGL white color. */
static inline lv_color_t lv_color_white(void)
{
    return UINT32_C(0xFFFFFF);
}

/**
 * @brief Generate no-op LVGL compatibility functions for layout-only tests.
 * @note Generated names intentionally mirror the external LVGL API.
 */
#define HOST_LV_NOOP_2(name, type1, type2) \
    static inline void name(type1 first, type2 second) \
    { \
        (void)first; \
        (void)second; \
    }

#define HOST_LV_NOOP_3(name, type1, type2, type3) \
    static inline void name(type1 first, type2 second, type3 third) \
    { \
        (void)first; \
        (void)second; \
        (void)third; \
    }

#define HOST_LV_NOOP_4(name, type1, type2, type3, type4) \
    static inline void name(type1 first, type2 second, type3 third, type4 fourth) \
    { \
        (void)first; \
        (void)second; \
        (void)third; \
        (void)fourth; \
    }

HOST_LV_NOOP_2(lv_obj_set_flex_flow, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_flex_grow, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_scroll_dir, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_scrollbar_mode, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_label_set_long_mode, lv_obj_t *, int)
HOST_LV_NOOP_3(lv_obj_set_style_bg_color, lv_obj_t *, lv_color_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_border_width, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_all, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_bottom, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_column, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_gap, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_left, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_right, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_row, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_pad_top, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_radius, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_shadow_width, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_text_align, lv_obj_t *, int, int)
HOST_LV_NOOP_3(lv_obj_set_style_text_color, lv_obj_t *, lv_color_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_text_font, lv_obj_t *, const lv_font_t *, int)
HOST_LV_NOOP_3(lv_obj_set_style_text_line_space, lv_obj_t *, int32_t, int)
HOST_LV_NOOP_4(lv_obj_align, lv_obj_t *, int, int32_t, int32_t)
HOST_LV_NOOP_4(lv_obj_set_flex_align, lv_obj_t *, int, int, int)

/** @brief Center a fake object. */
static inline void lv_obj_center(lv_obj_t *object)
{
    (void)object;
}

#undef HOST_LV_NOOP_2
#undef HOST_LV_NOOP_3
#undef HOST_LV_NOOP_4

#ifdef __cplusplus
}
#endif /* __CROSS_LAYER_LVGL_H__ */

#endif

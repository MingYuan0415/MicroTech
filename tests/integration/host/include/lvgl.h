/** @file Minimal LVGL API model used by integration host tests. */
#ifndef __CROSS_LAYER_LVGL_H__
#define __CROSS_LAYER_LVGL_H__

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

/** @brief Fake LVGL font object. */
typedef struct lv_font_t
{
    uint8_t marker;
} lv_font_t;

typedef uint32_t lv_color_t;
typedef int32_t lv_event_code_t;
/** @brief Fake LVGL event callback. */
typedef void (*lv_event_cb_t)(lv_event_t *event);
/** @brief Fake LVGL timer callback. */
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);

extern const lv_font_t host_lv_default_font;

#define LV_FONT_DEFAULT (&host_lv_default_font)

#define LV_ALIGN_TOP_LEFT             0
#define LV_ANIM_OFF                   0
#define LV_DIR_VER                    1
#define LV_EVENT_CLICKED              1
#define LV_EVENT_VALUE_CHANGED        2
#define LV_EVENT_RELEASED             3
#define LV_FLEX_ALIGN_START           0
#define LV_FLEX_ALIGN_CENTER          1
#define LV_FLEX_ALIGN_SPACE_BETWEEN   2
#define LV_FLEX_FLOW_COLUMN           0
#define LV_FLEX_FLOW_ROW              1
#define LV_LABEL_LONG_DOT             0
#define LV_LABEL_LONG_WRAP            1
#define LV_OBJ_FLAG_SCROLLABLE        1
#define LV_OPA_COVER                  255
#define LV_PART_MAIN                  0
#define LV_PART_INDICATOR             1
#define LV_PART_KNOB                  2
#define LV_SCROLLBAR_MODE_AUTO        0
#define LV_SIZE_CONTENT               (-1)
#define LV_STATE_PRESSED              1
#define LV_TEXT_ALIGN_CENTER          0

#define LV_PCT(value) (value)

#define LV_SYMBOL_EYE_OPEN  "eye"
#define LV_SYMBOL_BACKSPACE "backspace"
#define LV_SYMBOL_CLOSE     "close"
#define LV_SYMBOL_HOME      "home"
#define LV_SYMBOL_LEFT      "left"
#define LV_SYMBOL_LIST      "list"
#define LV_SYMBOL_POWER     "power"
#define LV_SYMBOL_REFRESH   "refresh"
#define LV_SYMBOL_RIGHT     "right"
#define LV_SYMBOL_SETTINGS  "settings"
#define LV_SYMBOL_WIFI      "wifi"

/** @brief Return the fake active screen. */
lv_obj_t *lv_screen_active(void);
/** @brief Create a fake generic object. */
lv_obj_t *lv_obj_create(lv_obj_t *parent);
/** @brief Create a fake button. */
lv_obj_t *lv_button_create(lv_obj_t *parent);
/** @brief Create a fake label. */
lv_obj_t *lv_label_create(lv_obj_t *parent);
/** @brief Create a fake slider. */
lv_obj_t *lv_slider_create(lv_obj_t *parent);
/** @brief Delete a fake object. */
void lv_obj_delete(lv_obj_t *object);
/** @brief Delete all children from a fake object. */
void lv_obj_clean(lv_obj_t *object);
/** @brief Register a fake event callback. */
void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback,
                         lv_event_code_t code, void *user_data);

/** @brief Return a fake event code. */
lv_event_code_t lv_event_get_code(lv_event_t *event);
/** @brief Return fake event user data. */
void *lv_event_get_user_data(lv_event_t *event);

/** @brief Set fake label text. */
void lv_label_set_text(lv_obj_t *label, const char *text);
/** @brief Set formatted fake label text. */
void lv_label_set_text_fmt(lv_obj_t *label, const char *format, ...);
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
/** @brief Return fake timer user data. */
void *lv_timer_get_user_data(lv_timer_t *timer);

/** @brief Convert a host RGB value to the fake LVGL color type. */
static inline lv_color_t lv_color_hex(uint32_t color)
{
    return color;
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

HOST_LV_NOOP_2(lv_obj_remove_flag, lv_obj_t *, uint32_t)
HOST_LV_NOOP_2(lv_obj_set_flex_flow, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_flex_grow, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_height, lv_obj_t *, int32_t)
HOST_LV_NOOP_2(lv_obj_set_scroll_dir, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_scrollbar_mode, lv_obj_t *, int)
HOST_LV_NOOP_2(lv_obj_set_width, lv_obj_t *, int32_t)
HOST_LV_NOOP_2(lv_label_set_long_mode, lv_obj_t *, int)
HOST_LV_NOOP_3(lv_obj_set_size, lv_obj_t *, int32_t, int32_t)
HOST_LV_NOOP_3(lv_obj_set_style_bg_color, lv_obj_t *, lv_color_t, int)
HOST_LV_NOOP_3(lv_obj_set_style_bg_opa, lv_obj_t *, int, int)
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

/** @brief Remove fake object styles. */
static inline void lv_obj_remove_style_all(lv_obj_t *object)
{
    (void)object;
}

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

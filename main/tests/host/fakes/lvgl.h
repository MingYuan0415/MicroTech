#ifndef __LVGL_H__
#define __LVGL_H__

#include <stdint.h>

/** @brief Incomplete host-test LVGL font type. */
typedef struct lv_font lv_font_t;
/** @brief Incomplete host-test LVGL object type. */
typedef struct lv_obj lv_obj_t;
/** @brief Incomplete host-test LVGL image descriptor type. */
typedef struct lv_image_dsc lv_image_dsc_t;

/** @brief Minimal LVGL result values used by draw-buffer declarations. */
typedef enum
{
    LV_RESULT_OK = 0,
    LV_RESULT_INVALID,
} lv_result_t;

/** @brief Minimal host-test LVGL draw buffer exposed by recent tasks. */
typedef struct lv_draw_buf
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t color_format;
    uint32_t data_size;
    const void *data;
} lv_draw_buf_t;

#endif /* __LVGL_H__ */

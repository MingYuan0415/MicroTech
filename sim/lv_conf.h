/**
 * @file Simulator lv_conf.h for the MicroTech LVGL host simulator.
 *
 * Hand-aligned with the production device profile: every `LV_*` below mirrors
 * a `CONFIG_LV_*` entry of the generated `build/sim/gen_inc/sdkconfig.h`.
 * `sim/ci/check_lv_conf.py` enforces lv_conf.h <-> sdkconfig.h
 * <-> sdkconfig.defaults agreement, including LV_USE_CANVAS, LV_USE_SNAPSHOT,
 * LV_USE_IMAGE, LV_FREETYPE_USE_LVGL_PORT=1 and LV_USE_FS_POSIX=0.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- Memory / OS (root CMakeLists gates; production LV_OS_NONE) ---- */
#define LV_USE_OS                       LV_OS_NONE
#define LV_USE_CLIB_MALLOC              1
#define LV_USE_CLIB_STRING              1
#define LV_USE_CLIB_SPRINTF             1
/* LVGL 9 consumes the STDLIB selectors; the device reaches them through
 * lv_conf_kconfig.h mappings of the CLIB_* Kconfig symbols. With
 * LV_KCONFIG_IGNORE (desktop build) the mapping is compiled out, so the
 * sim must set the resolved values directly here. */
#define LV_USE_STDLIB_MALLOC            LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING            LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF           LV_STDLIB_CLIB

/* ---- Color (root CMakeLists enforces RGB565 on device) ---- */
#define LV_COLOR_DEPTH                  16

/* ---- Default font (sdkconfig.defaults:102) ---- */
#define LV_FONT_DEFAULT                 &lv_font_montserrat_18

/* ---- Software renderer ASM: LV_USE_DRAW_SW_ASM 0 mirrors the device
 * CONFIG_LV_USE_DRAW_SW_ASM (= LV_DRAW_SW_ASM_NONE) in the block below. ---- */

/* ---- Filesystem: F: is registered by the sim shim lv_fs_drv, never by the
 * POSIX driver (it would claim the same drive letter as the mmap FS). ---- */
#define LV_USE_FS_POSIX                 0

/* ---- Remaining values mirrored one-to-one from the device sdkconfig ---- */
#define LV_DEF_REFR_PERIOD                           15
#define LV_DPI_DEF                                   130
#define LV_DRAW_BUF_STRIDE_ALIGN                     1
#define LV_DRAW_BUF_ALIGN                            4
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE                24576
#define LV_DRAW_LAYER_MAX_MEMORY                     0
#define LV_USE_DRAW_SW                               1
#define LV_DRAW_SW_SUPPORT_RGB565                    1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED            1
#define LV_DRAW_SW_SUPPORT_RGB565A8                  1
#define LV_DRAW_SW_SUPPORT_RGB888                    1
#define LV_DRAW_SW_SUPPORT_XRGB8888                  1
#define LV_DRAW_SW_SUPPORT_ARGB8888                  1
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED    1
#define LV_DRAW_SW_SUPPORT_L8                        1
#define LV_DRAW_SW_SUPPORT_AL88                      1
#define LV_DRAW_SW_SUPPORT_A8                        1
#define LV_DRAW_SW_SUPPORT_I1                        1
#define LV_DRAW_SW_I1_LUM_THRESHOLD                  127
#define LV_DRAW_SW_DRAW_UNIT_CNT                     1
#define LV_DRAW_SW_COMPLEX                           1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE                 0
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE                 4
#define LV_USE_DRAW_SW_ASM                           0
#define LV_USE_ASSERT_NULL                           1
#define LV_USE_ASSERT_MALLOC                         1
#define LV_ASSERT_HANDLER_INCLUDE                    "assert.h"
#define LV_CACHE_DEF_SIZE                            0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT                0
#define LV_GRADIENT_MAX_STOPS                        2
#define LV_COLOR_MIX_ROUND_OFS                       128
#define LV_OBJ_STYLE_CACHE                           1
#define LV_USE_GESTURE_RECOGNITION                   1
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE                  1
#define LV_USE_FLOAT                                 1
#define LV_FONT_MONTSERRAT_14                        1
#define LV_FONT_MONTSERRAT_18                        1
#define LV_USE_FONT_PLACEHOLDER                      1
#define LV_TXT_ENC_UTF8                              1
#define LV_TXT_BREAK_CHARS                           " ,.;:-_)}"
#define LV_TXT_LINE_BREAK_LONG_LEN                   0
#define LV_TXT_COLOR_CMD                             "#"
#define LV_WIDGETS_HAS_DEFAULT_VALUE                 1
#define LV_USE_ANIMIMG                               1
#define LV_USE_ARC                                   1
#define LV_USE_ARCLABEL                              1
#define LV_USE_BAR                                   1
#define LV_USE_BUTTON                                1
#define LV_USE_BUTTONMATRIX                          1
#define LV_USE_CALENDAR                              1
#define LV_MONDAY_STR                                "Mo"
#define LV_TUESDAY_STR                               "Tu"
#define LV_WEDNESDAY_STR                             "We"
#define LV_THURSDAY_STR                              "Th"
#define LV_FRIDAY_STR                                "Fr"
#define LV_SATURDAY_STR                              "Sa"
#define LV_SUNDAY_STR                                "Su"
#define LV_USE_CALENDAR_HEADER_ARROW                 1
#define LV_USE_CALENDAR_HEADER_DROPDOWN              1
#define LV_USE_CANVAS                                1
#define LV_USE_CHART                                 1
#define LV_USE_CHECKBOX                              1
#define LV_USE_DROPDOWN                              1
#define LV_USE_IMAGE                                 1
#define LV_USE_IMAGEBUTTON                           1
#define LV_USE_KEYBOARD                              1
#define LV_USE_LABEL                                 1
#define LV_LABEL_TEXT_SELECTION                      1
#define LV_LABEL_LONG_TXT_HINT                       1
#define LV_LABEL_WAIT_CHAR_COUNT                     3
#define LV_USE_LED                                   1
#define LV_USE_LINE                                  1
#define LV_USE_LIST                                  1
#define LV_USE_MENU                                  1
#define LV_USE_MSGBOX                                1
#define LV_USE_ROLLER                                1
#define LV_USE_SCALE                                 1
#define LV_USE_SLIDER                                1
#define LV_USE_SPAN                                  1
#define LV_SPAN_SNIPPET_STACK_SIZE                   64
#define LV_USE_SPINBOX                               1
#define LV_USE_SPINNER                               1
#define LV_USE_SWITCH                                1
#define LV_USE_TEXTAREA                              1
#define LV_TEXTAREA_DEF_PWD_SHOW_TIME                1500
#define LV_USE_TABLE                                 1
#define LV_USE_TABVIEW                               1
#define LV_USE_TILEVIEW                              1
#define LV_USE_WIN                                   1
#define LV_USE_THEME_DEFAULT                         1
#define LV_THEME_DEFAULT_GROW                        1
#define LV_THEME_DEFAULT_TRANSITION_TIME             80
#define LV_USE_THEME_SIMPLE                          1
#define LV_USE_FLEX                                  1
#define LV_USE_GRID                                  1
#define LV_FS_DEFAULT_DRIVER_LETTER                  0
#define LV_USE_QRCODE                                1
#define LV_USE_FREETYPE                              1
#define LV_FREETYPE_USE_LVGL_PORT                    1
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT               256
#define LV_USE_SNAPSHOT                              1
#define LV_USE_OBSERVER                              1

#endif /* LV_CONF_H */

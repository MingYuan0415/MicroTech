#ifndef __CROSS_LAYER_RUNTIME_H__
#define __CROSS_LAYER_RUNTIME_H__

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

/** @brief Snapshot of one live object owned by the fake LVGL system layer. */
typedef struct host_lv_system_object_snapshot
{
    const lv_obj_t *object;
    bool visible;
    lv_obj_flag_t flags;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    lv_opa_t opacity;
    lv_opa_t background_opacity;
    lv_opa_t text_opacity;
    bool canvas;
    lv_color_format_t canvas_color_format;
    lv_color_t image_recolor;
    lv_opa_t image_opacity;
    uint32_t canvas_flush_count;
    uint32_t invalidation_count;
    const char *text;
} host_lv_system_object_snapshot_t;

/** @brief Reset all host service and LVGL fixtures. */
void host_runtime_reset(void);
/** @brief Stop and join the mailbox worker task. */
void host_task_shutdown(void);

/** @brief Reset the fixed-capacity LVGL object model. */
void host_lv_reset(void);
/** @brief Return the singleton fake pointer input device. */
lv_indev_t *host_lv_pointer_indev(void);
/**
 * @brief Click the visible action containing an exact title.
 * @param title is the action label to match.
 * @return true when an action was clicked; false when not found.
 */
bool host_lv_click_action(const char *title);
/** @brief Click the visible back action, if present. */
bool host_lv_click_back(void);
/** @brief Start one pointer sequence at a logical display coordinate. */
bool host_lv_touch_press(int32_t x, int32_t y);
/** @brief Move the active fake pointer sequence. */
bool host_lv_touch_move(int32_t x, int32_t y);
/** @brief Release the active fake pointer sequence. */
bool host_lv_touch_release(int32_t x, int32_t y);
/** @brief Reset the fake pointer and notify its active object. */
void host_lv_touch_reset(void);
/**
 * @brief Snapshot the object which owns the active pointer sequence.
 * @param snapshot receives identity, geometry and style state.
 * @return true while a live pointer target exists; false otherwise.
 */
bool host_lv_pointer_target_snapshot(
    host_lv_system_object_snapshot_t *snapshot);
/**
 * @brief Snapshot the fake display's active Screen.
 * @param snapshot receives identity, geometry and style state.
 * @return true when the active Screen is live; false otherwise.
 */
bool host_lv_active_screen_snapshot(
    host_lv_system_object_snapshot_t *snapshot);
/**
 * @brief Report whether an exact label is currently visible.
 * @param text is the label text to match.
 * @return true when visible; false otherwise.
 */
bool host_lv_has_text(const char *text);
/**
 * @brief Report whether the pending transition target contains exact text.
 * @param text is the label text to match.
 * @return true when the target Screen contains the label; false otherwise.
 */
bool host_lv_transition_target_has_text(const char *text);
/** @brief Return the number of live LVGL objects excluding the root screen. */
size_t host_lv_live_object_count(void);
/** @brief Return the number of live dynamically created LVGL screens. */
size_t host_lv_live_screen_count(void);
/** @brief Return the number of live LVGL timers. */
size_t host_lv_live_timer_count(void);
/** @brief Return the number of live objects below the fake system layer. */
size_t host_lv_system_object_count(void);
/**
 * @brief Snapshot one live fake system-layer descendant by allocation order.
 * @param index is in the range [0, host_lv_system_object_count()).
 * @param snapshot receives borrowed text and current geometry/style state.
 * @return true when the indexed object exists; false otherwise.
 */
bool host_lv_system_object_snapshot(
    size_t index, host_lv_system_object_snapshot_t *snapshot);
/**
 * @brief Find a visible system-layer label with exact text.
 * @param text is the exact label text.
 * @param snapshot optionally receives its current state.
 * @return true when found; false otherwise.
 */
bool host_lv_system_label_snapshot(
    const char *text, host_lv_system_object_snapshot_t *snapshot);
/**
 * @brief Read one A8 pixel from a live fake Canvas.
 * @param canvas is the Canvas identity returned by an object snapshot.
 * @param x is the zero-based Canvas-local horizontal coordinate.
 * @param y is the zero-based Canvas-local vertical coordinate.
 * @param alpha receives the stored A8 value.
 * @return true when the Canvas and coordinate are valid; false otherwise.
 */
bool host_lv_canvas_alpha_snapshot(const lv_obj_t *canvas, int32_t x,
                                   int32_t y, lv_opa_t *alpha);

#endif /* __CROSS_LAYER_RUNTIME_H__ */

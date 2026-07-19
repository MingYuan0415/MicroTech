#ifndef __CROSS_LAYER_RUNTIME_H__
#define __CROSS_LAYER_RUNTIME_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
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
/**
 * @brief Toggle all optional non-Wi-Fi service fakes.
 * @param available selects normal snapshots or unavailable responses.
 */
void host_optional_services_set_available(bool available);
/** @brief Return whether the fake RTC alarm is currently enabled. */
bool host_time_alarm_is_enabled(void);
/** @brief Return whether the fake SNTP request is owned by the clock page. */
bool host_time_sync_is_owned(void);
/** @brief Block or release the fake SNTP request inside its worker call. */
void host_time_sync_set_blocked(bool blocked);
/** @brief Return whether a blocked fake SNTP request has entered the service. */
bool host_time_sync_request_entered(void);
/**
 * @brief Publish one fake RTC alarm edge.
 * @param sequence is the nonzero alarm sequence.
 * @return ESP_OK when the event is admitted, otherwise an ESP-IDF error.
 */
esp_err_t host_time_publish_alarm(uint32_t sequence);
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
 * @brief Drag the unique visible, enabled Slider to a value.
 * @param value is clamped to the Slider's configured range.
 * @return true when LV_EVENT_VALUE_CHANGED was emitted; false otherwise.
 */
bool host_lv_visible_slider_drag(int32_t value);
/**
 * @brief Release the unique visible, enabled Slider after a drag.
 * @return true when LV_EVENT_RELEASED was emitted; false otherwise.
 */
bool host_lv_visible_slider_release(void);
/**
 * @brief Cancel the unique visible, enabled Slider after a drag.
 * @return true when LV_EVENT_PRESS_LOST was emitted; false otherwise.
 */
bool host_lv_visible_slider_cancel(void);
/**
 * @brief Snapshot the unique visible Slider.
 * @param value receives the current Slider value.
 * @param pressed receives whether LV_STATE_PRESSED is set.
 * @param disabled receives whether LV_STATE_DISABLED is set.
 * @return true when exactly one visible Slider exists; false otherwise.
 */
bool host_lv_visible_slider_snapshot(int32_t *value, bool *pressed,
                                     bool *disabled);
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
/** @brief Count visible labels whose text exactly matches the query. */
size_t host_lv_visible_text_count(const char *text);
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
/** @brief Mark every live LVGL timer ready and run it once. */
void host_lv_timer_step(void);
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

/** @brief Return the fake audio service's current volume. */
uint8_t host_audio_volume(void);
/** @brief Return the number of successful fake audio read calls. */
unsigned host_audio_read_count(void);
/** @brief Return the number of valid fake set-volume calls. */
unsigned host_audio_set_volume_count(void);
/** @brief Set the PCM peak returned by subsequent fake audio reads. */
void host_audio_set_read_peak(int16_t peak);
/** @brief Fail the next valid fake set-volume call. */
void host_audio_fail_next_volume(void);

#endif /* __CROSS_LAYER_RUNTIME_H__ */

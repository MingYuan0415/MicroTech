#ifndef __CROSS_LAYER_RUNTIME_H__
#define __CROSS_LAYER_RUNTIME_H__

#include <stdbool.h>

#include "lvgl.h"

/** @brief Reset all host service and LVGL fixtures. */
void host_runtime_reset(void);
/** @brief Stop and join the mailbox worker task. */
void host_task_shutdown(void);

/** @brief Reset the fixed-capacity LVGL object model. */
void host_lv_reset(void);
/**
 * @brief Click the visible action containing an exact title.
 * @param title is the action label to match.
 * @return true when an action was clicked; false when not found.
 */
bool host_lv_click_action(const char *title);
/** @brief Click the visible back action, if present. */
bool host_lv_click_back(void);
/**
 * @brief Report whether an exact label is currently visible.
 * @param text is the label text to match.
 * @return true when visible; false otherwise.
 */
bool host_lv_has_text(const char *text);

#endif /* __CROSS_LAYER_RUNTIME_H__ */

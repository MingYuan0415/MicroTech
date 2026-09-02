/** @file Simulator-specific controls for the esp_lv_adapter shim. */
#ifndef SIM_LV_ADAPTER_H
#define SIM_LV_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_lv_adapter.h"

/** @brief Update the atomic pointer state consumed by the indev read cb.
 *  Coordinates are panel pixels (0..367, 0..447). */
void sim_lv_touch_update(int16_t x, int16_t y, bool pressed);
/** @brief CI step gating: after mailbox s_ready only. */
void sim_lv_ci_mode_set(bool enabled);
/** @brief Advance the explicit tick by `ms` and drain all ready timers.
 *  Returns when the worker finished the step (ack). Only valid in CI mode. */
int sim_lv_ci_step(uint32_t ms);
/** @brief True when CI step gating is active. */
bool sim_lv_ci_enabled(void);
/** @brief True when the LVGL worker is paused behind its display fence. */
bool sim_lv_paused(void);
/** @brief Drain until no LVGL timer is ready without advancing a step. */
int sim_lv_ci_settle(void);
/** @brief Monotonic blit counter (one increment per flushed strip). */
uint64_t sim_lv_frame_count(void);
/** @brief The registered display (NULL before registration). */
lv_display_t *sim_lv_display(void);

#endif /* SIM_LV_ADAPTER_H */

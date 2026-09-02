/** @file LCD type declarations aligned with the ESP-IDF opaque handles. */
#ifndef __SIM_ESP_LCD_TYPES_H__
#define __SIM_ESP_LCD_TYPES_H__

#include "esp_err.h"

/** @brief Panel object; the draw_bitmap slot is installed by sim_bsp. */
typedef struct esp_lcd_panel_t esp_lcd_panel_t;

/** @brief Host LCD panel handle, matching the IDF opaque pointer shape. */
typedef esp_lcd_panel_t *esp_lcd_panel_handle_t;

/** @brief Opaque host LCD transport handle. */
typedef void *esp_lcd_panel_io_handle_t;

#endif /* __SIM_ESP_LCD_TYPES_H__ */

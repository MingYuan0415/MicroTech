/** @file Simulated board: 368x448 RGB565 framebuffer with an LCD panel
 *  whose draw_bitmap is the single pixel write path (blit into the FB),
 *  plus display-port/touch/input/screen contracts from layers/bsp's
 *  bsp_hal.h (implemented without the real BSP sources).
 */
#ifndef SIM_BSP_H
#define SIM_BSP_H

#include <stdbool.h>
#include <stdint.h>

#include "app_manager_types.h"
#include "bsp_hal.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define SIM_BSP_H_RES 368U
#define SIM_BSP_V_RES 448U

/** @brief Logical keys mirrored from the board contract. */
typedef enum
{
    SIM_KEY_BOOT = 0,
    SIM_KEY_POWER,
} sim_key_t;

typedef enum
{
    SIM_KEY_ACTION_PRESS = 0,
    SIM_KEY_ACTION_RELEASE,
} sim_key_action_t;

/** @brief Initialize the simulated panel and framebuffer. */
esp_err_t sim_bsp_init(void);
/** @brief Release framebuffer resources. */
void sim_bsp_deinit(void);
/** @brief Return the RGB565 framebuffer (SIM_BSP_H_RES * SIM_BSP_V_RES). */
const uint16_t *sim_bsp_framebuffer(void);
/** @brief Return the panel handle for bsp_display_port_t wiring. */
esp_lcd_panel_handle_t sim_bsp_panel(void);
/** @brief Return the display port contract for app_manager_config_t. */
const bsp_display_port_t *sim_bsp_display_port(void);
/** @brief Return screen lifecycle ops for app_manager_config_t. */
const bsp_screen_ops_t *sim_bsp_screen_ops(void);
/** @brief Bit mask satisfying the app_runtime capability gate. */
bsp_capabilities_t sim_bsp_capabilities(void);
/** @brief Inject one physical key edge into the input bridge. */
esp_err_t sim_bsp_key(sim_key_t key, sim_key_action_t action);
/** @brief Register the app-manager input callback bridge (once). */
esp_err_t sim_bsp_input_register(void (*cb)(app_manager_key_t key,
                                 app_manager_key_event_t event,
                                 void *user_data),
                                 void *user_data);
/** @brief Suspend screen state observer for standby fake data. */
bool sim_bsp_screen_is_suspended(void);
/** @brief Current brightness byte stored by the screen ops fake. */
uint8_t sim_bsp_brightness(void);
/** @brief Return the cumulative number of draw_bitmap calls. */
uint32_t sim_bsp_blit_count(void);
/** @brief Reset blit statistics. */
void sim_bsp_reset_stats(void);

#endif /* SIM_BSP_H */

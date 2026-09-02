/** @file Minimal GPIO declarations used by the simulator. */
#ifndef __SIM_DRIVER_GPIO_H__
#define __SIM_DRIVER_GPIO_H__

#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)

/** @brief Disable one fake GPIO interrupt. */
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
/** @brief Read one fake GPIO level (always low). */
int gpio_get_level(gpio_num_t gpio_num);

#endif /* __SIM_DRIVER_GPIO_H__ */

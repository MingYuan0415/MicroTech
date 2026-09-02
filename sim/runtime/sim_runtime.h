/** @file Simulator runtime assembly API. */
#ifndef SIM_RUNTIME_H
#define SIM_RUNTIME_H

#include "esp_err.h"

/** @brief Boot services, app_manager, and commit the initial app frame. */
esp_err_t sim_runtime_boot(void);

#endif /* SIM_RUNTIME_H */

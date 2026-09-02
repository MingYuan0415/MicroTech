/** @file Simulator time port controls (CI injection). */
#ifndef SIM_TIME_H
#define SIM_TIME_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief Install a virtual epoch and notify the time service. */
esp_err_t sim_time_set_epoch(int64_t epoch_seconds);
/** @return the virtual epoch (-1 when following the host clock). */
int64_t sim_time_port_epoch(void);
/** @brief Stop overriding the clock; host time becomes authoritative. */
esp_err_t sim_time_follow_host(void);
/** @brief Disable the fake SNTP async callback (CI uses injected epoch). */
void sim_time_set_sntp_enabled(bool enabled);

#endif /* SIM_TIME_H */

/** @file Scriptable hardware backends (power/imu) and sim hooks. */
#ifndef SIM_BACKENDS_H
#define SIM_BACKENDS_H

#include <stdbool.h>
#include <stdint.h>

#include "imu_service.h"
#include "power_service.h"

extern const power_service_power_ops_t sim_power_ops;
extern const imu_service_imu_ops_t sim_imu_ops;

/** @brief Agent sim.set_power backend. */
void sim_backends_set_power(uint16_t voltage_mv, int8_t percent,
                            bool charging, bool vbus);
/** @brief Agent sim.set_imu backend (centidegrees). */
void sim_backends_set_imu(int pitch_cdeg, int roll_cdeg);
/** @brief factory_reset restart hook (logs; process continues). */
void sim_backends_restart(void *context);

#endif /* SIM_BACKENDS_H */

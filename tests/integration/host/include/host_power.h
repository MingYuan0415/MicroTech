/** @file Power fake overrides for cross-layer host tests. */
#ifndef __CROSS_LAYER_HOST_POWER_H__
#define __CROSS_LAYER_HOST_POWER_H__

#include "power_service.h"

/** @brief Override the power snapshot returned by the fake service. */
void host_power_set_snapshot(const power_service_snapshot_t *snapshot);

#endif /* __CROSS_LAYER_HOST_POWER_H__ */

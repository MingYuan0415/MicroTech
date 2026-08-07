#ifndef __HOST_BLE_NIMBLE_PORT_H__
#define __HOST_BLE_NIMBLE_PORT_H__

#include "ble_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Host fake: returns the test runtime port (no NimBLE). */
const ble_runtime_host_port_t *ble_nimble_port_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_BLE_NIMBLE_PORT_H__ */

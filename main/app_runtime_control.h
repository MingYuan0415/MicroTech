#ifndef __APP_RUNTIME_CONTROL_H__
#define __APP_RUNTIME_CONTROL_H__

#include "app_manager_types.h"

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the control-center bridge from connectivity, Device Link, and
 *        time services.
 *
 * The getters are cheap and non-blocking; the setters only enqueue work in the
 * owning service (Bluetooth uses a zero wait). Every function pointer is always
 * populated; a service that is not yet ready makes its getter return an error,
 * which the surface renders as an unavailable tile.
 *
 * @return Application-manager control-center operations.
 */
app_manager_control_center_ops_t app_runtime_control_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_RUNTIME_CONTROL_H__ */
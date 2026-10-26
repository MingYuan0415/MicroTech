#ifndef __APP_RUNTIME_CONTROL_H__
#define __APP_RUNTIME_CONTROL_H__

#include "app_manager_types.h"

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the control-center bridge from connectivity, Device Link,
 *        time, and audio services.
 *
 * The getters are cheap and non-blocking. Bluetooth and brightness setters only
 * enqueue work in the owning service; set_volume performs the short codec
 * control transfer the audio service serializes, so the surface rate-limits it
 * while a drag is in progress, and set_volume_persisted asks audio_service to
 * store the level once the drag settles. Volume storage itself belongs to
 * audio_service; this bridge never touches it. Every function pointer is always
 * populated; a service that is not yet ready makes its getter return an error,
 * which the surface renders as an unavailable tile or row.
 *
 * @return Application-manager control-center operations.
 */
app_manager_control_center_ops_t app_runtime_control_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_RUNTIME_CONTROL_H__ */

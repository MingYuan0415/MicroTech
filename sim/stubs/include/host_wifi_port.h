/** @file Wi-Fi port fault controls and observations for host tests. */
#ifndef __WIFI_SERVICE_HOST_WIFI_PORT_H__
#define __WIFI_SERVICE_HOST_WIFI_PORT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_service_port.h"

/** @brief Operations accepted by the Wi-Fi port fault controller. */
typedef enum
{
    HOST_WIFI_PORT_INIT = 0,
    HOST_WIFI_PORT_DEINIT,
    HOST_WIFI_PORT_GET_STATE,
    HOST_WIFI_PORT_SCAN_IS_OWNED,
    HOST_WIFI_PORT_GET_EPOCH,
    HOST_WIFI_PORT_START,
    HOST_WIFI_PORT_STOP,
    HOST_WIFI_PORT_SCAN_START,
    HOST_WIFI_PORT_SCAN_FINISH,
    HOST_WIFI_PORT_SCAN_ABORT,
    HOST_WIFI_PORT_SET_CREDENTIALS,
    HOST_WIFI_PORT_CLEAR_CREDENTIALS,
    HOST_WIFI_PORT_CONNECT,
    HOST_WIFI_PORT_DISCONNECT,
    HOST_WIFI_PORT_OPERATION_COUNT,
} host_wifi_port_operation_t;

/** @brief Reset the fake Wi-Fi port. */
void host_wifi_port_reset(void);
/** @brief Fail selected future port calls. */
void host_wifi_port_fail_next(host_wifi_port_operation_t operation,
                              unsigned count, esp_err_t result);
/** @brief Set the default result for one port operation. */
void host_wifi_port_set_default_result(host_wifi_port_operation_t operation,
                                       esp_err_t result);
/** @brief Leave dirty ownership after the next deinit. */
void host_wifi_port_leave_dirty_after_next_deinit(void);
/** @brief Leave scan ownership after the next finish. */
void host_wifi_port_leave_scan_owned_after_next_finish(void);
/** @brief Preserve scan ownership after the next stop. */
void host_wifi_port_preserve_scan_on_next_stop(void);
/** @brief Leave partial ownership after the next start failure. */
void host_wifi_port_leave_partial_after_next_start_failure(void);

/** @brief Gate one port operation. */
void host_wifi_port_gate(host_wifi_port_operation_t operation, bool enabled);
/** @brief Wait until a port gate is reached. */
bool host_wifi_port_wait_gate(host_wifi_port_operation_t operation,
                              uint32_t timeout_ms);
/** @brief Release one port gate. */
void host_wifi_port_release_gate(host_wifi_port_operation_t operation);

/** @brief Return one port call count. */
unsigned host_wifi_port_call_count(host_wifi_port_operation_t operation);
/** @brief Wait for one port call count. */
bool host_wifi_port_wait_calls(host_wifi_port_operation_t operation,
                               unsigned expected, uint32_t timeout_ms);
/** @brief Return whether the fake port is clean. */
bool host_wifi_port_is_clean_snapshot(void);
/** @brief Return the fake port state. */
wifi_service_port_state_t host_wifi_port_state(void);
/** @brief Return whether a thread-affinity violation occurred. */
bool host_wifi_port_thread_violation(void);
/** @brief Return whether scan ownership was violated. */
bool host_wifi_port_scan_ownership_violation(void);
/** @brief Return whether a scan is owned. */
bool host_wifi_port_scan_owned(void);
/** @brief Return the fake port epoch. */
uint64_t host_wifi_port_epoch(void);
/** @brief Return the fake scan identifier. */
uint8_t host_wifi_port_scan_id(void);

/** @brief Configure fake scan records. */
void host_wifi_port_set_scan_records(
    const wifi_service_port_scan_record_t *records, size_t count,
    bool truncated);
/** @brief Copy the last credentials observed by the fake port. */
bool host_wifi_port_last_credentials(
    wifi_service_port_credentials_t *credentials);

#endif /* __WIFI_SERVICE_HOST_WIFI_PORT_H__ */

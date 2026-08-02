/** @file In-memory NVS controls for connectivity manager tests. */
#ifndef __CONNECTIVITY_HOST_NV_STORAGE_H__
#define __CONNECTIVITY_HOST_NV_STORAGE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Reset the in-memory profile and all injected failures. */
void host_nv_storage_reset(void);
/** @brief Fail the next profile read. */
void host_nv_storage_fail_next_get(esp_err_t result);
/** @brief Fail the next profile write without replacing the old value. */
void host_nv_storage_fail_next_set(esp_err_t result);
/** @brief Fail the next profile erase. */
void host_nv_storage_fail_next_erase(esp_err_t result);
/** @brief Replace storage with an arbitrary record for validation tests. */
void host_nv_storage_seed(const void *data, size_t size);
/** @brief Copy the current record and report whether it exists. */
bool host_nv_storage_copy(void *data, size_t capacity, size_t *size);
/** @brief Return the number of successful profile writes. */
unsigned host_nv_storage_set_count(void);
/** @brief Return the number of successful profile erases. */
unsigned host_nv_storage_erase_count(void);

#endif /* __CONNECTIVITY_HOST_NV_STORAGE_H__ */

#ifndef __FS_STORAGE_H__
#define __FS_STORAGE_H__

#include "esp_err.h"

#include <stdbool.h>

/** @brief Read-only application resource mount path. */
#define FS_RES_MOUNT_PATH "/res"
/** @brief Runtime data filesystem mount path. */
#define FS_DATA_MOUNT_PATH "/data"

/**
 * @brief Mount required runtime data and the optional legacy resource VFS.
 *
 * @note When the LVGL mmap backend owns resources, no resource VFS is mounted.
 *       In legacy mode, a missing read-only resource partition is degradable;
 *       the runtime data filesystem remains mandatory.
 *
 * @return ESP_OK when runtime data is ready, otherwise an ESP-IDF error.
 *
 * @warning The application runtime must serialize initialization and cleanup.
 */
esp_err_t fs_storage_init(void);

/**
 * @brief Unmount every owned filesystem in reverse order.
 *
 * @return ESP_OK when all filesystems unmount, otherwise an ESP-IDF error.
 *
 * @warning The application runtime must serialize initialization and cleanup.
 */
esp_err_t fs_storage_deinit(void);

/**
 * @brief Report whether the required data filesystem is mounted.
 *
 * @return true when initialized; false otherwise.
 */
bool fs_storage_is_initialized(void);

#endif

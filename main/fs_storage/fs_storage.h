#ifndef __FS_STORAGE_H__
#define __FS_STORAGE_H__

#include "esp_err.h"

#include <stdbool.h>

/** @brief Read-only application resource mount path. */
#define FS_RES_MOUNT_PATH "/res"
/** @brief Runtime data filesystem mount path. */
#ifndef FS_DATA_MOUNT_PATH
    #define FS_DATA_MOUNT_PATH "/data"
#endif

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

/**
 * @brief Delete every file and subdirectory under the runtime data mount.
 *
 * Factory reset recovery calls this before services start so cached
 * application data (weather and future per-app files) cannot survive the
 * reset. The mount point itself remains; only its contents are removed.
 *
 * @return ESP_OK when the data directory holds no entries, otherwise an
 *         ESP-IDF or filesystem error.
 *
 * @warning The application runtime must serialize this with initialization
 *          and cleanup.
 */
esp_err_t fs_storage_wipe_data(void);

#endif

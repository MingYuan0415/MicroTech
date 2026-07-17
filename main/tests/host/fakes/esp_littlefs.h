#ifndef __ESP_LITTLEFS_H__
#define __ESP_LITTLEFS_H__

#include "esp_err.h"

#include <stdbool.h>

/** @brief Minimal LittleFS mount configuration used by host tests. */
typedef struct esp_vfs_littlefs_conf
{
    const char *base_path;       /**< VFS mount path. */
    const char *partition_label; /**< Partition label. */
    bool format_if_mount_failed; /**< Permit implicit formatting. */
    bool dont_mount;             /**< Register without mounting. */
} esp_vfs_littlefs_conf_t;

/**
 * @brief Register the fake LittleFS VFS mount.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_vfs_littlefs_register(
    const esp_vfs_littlefs_conf_t *config);
/**
 * @brief Unregister the fake LittleFS VFS mount.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
/**
 * @brief Format the fake LittleFS partition.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_littlefs_format(const char *partition_label);

#endif /* __ESP_LITTLEFS_H__ */

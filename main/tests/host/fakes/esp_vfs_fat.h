#ifndef __ESP_VFS_FAT_H__
#define __ESP_VFS_FAT_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief Minimal read-only FAT mount configuration used by host tests. */
typedef struct esp_vfs_fat_mount_config
{
    bool format_if_mount_failed;   /**< Permit implicit formatting. */
    int max_files;                 /**< Maximum simultaneously open files. */
    size_t allocation_unit_size;   /**< Requested allocation unit size. */
    bool disk_status_check_enable; /**< Enable media status checks. */
    bool use_one_fat;              /**< Use one FAT table. */
} esp_vfs_fat_mount_config_t;

/**
 * @brief Mount the scripted read-only FAT partition.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_vfs_fat_spiflash_mount_ro(
    const char *base_path, const char *partition_label,
    const esp_vfs_fat_mount_config_t *config);
/**
 * @brief Unmount the scripted read-only FAT partition.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_vfs_fat_spiflash_unmount_ro(
    const char *base_path, const char *partition_label);

#endif /* __ESP_VFS_FAT_H__ */

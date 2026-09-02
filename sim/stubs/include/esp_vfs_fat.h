/** @file FAT filesystem compatibility declarations for integration tests. */
#ifndef __CROSS_LAYER_ESP_VFS_FAT_H__
#define __CROSS_LAYER_ESP_VFS_FAT_H__

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Return deterministic capacity data for the fake mounted filesystem.
 * @param base_path is the expected fake mount path.
 * @param total_bytes receives total filesystem capacity.
 * @param free_bytes receives available filesystem capacity.
 * @return ESP_OK when all arguments identify the fake mount.
 */
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total_bytes,
                           uint64_t *free_bytes);

#endif /* __CROSS_LAYER_ESP_VFS_FAT_H__ */

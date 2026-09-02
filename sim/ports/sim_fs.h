/** @file LVGL 'F:' resource filesystem driver registration. */
#ifndef SIM_FS_H
#define SIM_FS_H

#include "esp_err.h"
#include "esp_mmap_assets.h"

/** @brief Register an lv_fs_drv_t mapping the drive letter to mmap assets. */
esp_err_t sim_fs_mount(char letter, mmap_assets_handle_t assets);
/** @brief Forget the mounted drive (deinit path). */
void sim_fs_unmount(void);

#endif /* SIM_FS_H */

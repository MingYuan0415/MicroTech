#define DBG_TAG "fs_storage"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "fs_storage.h"

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define FS_STORAGE_RES_PARTITION  "res"
#define FS_STORAGE_DATA_PARTITION "data"
#define FS_STORAGE_PROBE_BYTES    4096U

typedef enum
{
    FS_STORAGE_UNINITIALIZED = 0,
    FS_STORAGE_INITIALIZING,
    FS_STORAGE_READY,
    FS_STORAGE_DEINITIALIZING,
} fs_storage_state_t;

static atomic_int s_state = ATOMIC_VAR_INIT(FS_STORAGE_UNINITIALIZED);
static bool s_res_mounted;
static bool s_data_mounted;

static esp_err_t _fs_storage_data_partition_is_erased(bool *is_erased)
{
    esp_err_t result = ESP_OK;
    *is_erased = false;
    uint8_t *partition_probe = heap_caps_malloc_prefer(
                                   FS_STORAGE_PROBE_BYTES, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (partition_probe == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    const esp_partition_t *partition = esp_partition_find_first(
                                           ESP_PARTITION_TYPE_DATA,
                                           ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
                                           FS_STORAGE_DATA_PARTITION);
    if (partition == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    for (size_t offset = 0; offset < partition->size;)
    {
        size_t remaining = partition->size - offset;
        size_t read_size = remaining < FS_STORAGE_PROBE_BYTES ?
                           remaining : FS_STORAGE_PROBE_BYTES;
        result = esp_partition_read(partition, offset, partition_probe,
                                    read_size);
        if (result != ESP_OK)
        {
            goto exit;
        }
        for (size_t index = 0; index < read_size; index++)
        {
            if (partition_probe[index] != UINT8_MAX)
            {
                goto exit;
            }
        }
        offset += read_size;
    }

    *is_erased = true;

exit:
    heap_caps_free(partition_probe);
    return result;
}

static esp_err_t _fs_storage_mount_data(
    const esp_vfs_littlefs_conf_t *config)
{
    esp_err_t result = esp_vfs_littlefs_register(config);
    if (result != ESP_FAIL)
    {
        goto exit;
    }

    bool is_erased = false;
    result = _fs_storage_data_partition_is_erased(&is_erased);
    if (result != ESP_OK)
    {
        goto exit;
    }
    if (!is_erased)
    {
        result = ESP_FAIL;
        goto exit;
    }

    LOG_W("provisioning erased data partition");
    result = esp_littlefs_format(FS_STORAGE_DATA_PARTITION);
    if (result == ESP_OK)
    {
        result = esp_vfs_littlefs_register(config);
    }

exit:
    return result;
}

static esp_err_t _fs_storage_unmount_owned(void)
{
    esp_err_t result = ESP_OK;
    if (s_data_mounted)
    {
        result = esp_vfs_littlefs_unregister(FS_STORAGE_DATA_PARTITION);
        s_data_mounted = result != ESP_OK;
    }

    if (result == ESP_OK && s_res_mounted)
    {
        result = esp_vfs_fat_spiflash_unmount_ro(
                     FS_RES_MOUNT_PATH, FS_STORAGE_RES_PARTITION);
        s_res_mounted = result != ESP_OK;
    }
    return result;
}

esp_err_t fs_storage_init(void)
{
    esp_err_t result = ESP_OK;
    int state = atomic_load(&s_state);
    if (state == FS_STORAGE_READY)
    {
        goto exit;
    }
    int expected = FS_STORAGE_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_state, &expected,
                                        FS_STORAGE_INITIALIZING))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    s_res_mounted = false;
    s_data_mounted = false;

#if !defined(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS) || \
    !CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    const esp_vfs_fat_mount_config_t res_config =
    {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    result = esp_vfs_fat_spiflash_mount_ro(
                 FS_RES_MOUNT_PATH, FS_STORAGE_RES_PARTITION,
                 &res_config);
    if (result == ESP_OK)
    {
        s_res_mounted = true;
    }
    else
    {
        LOG_W("resource partition unavailable: %s",
              esp_err_to_name(result));
    }
#else
    LOG_I("resource partition is owned by the LVGL mmap backend");
#endif

    const esp_vfs_littlefs_conf_t data_config =
    {
        .base_path = FS_DATA_MOUNT_PATH,
        .partition_label = FS_STORAGE_DATA_PARTITION,
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    result = _fs_storage_mount_data(&data_config);
    if (result != ESP_OK)
    {
        LOG_E("data partition mount failed: %s", esp_err_to_name(result));
        esp_err_t cleanup_result = _fs_storage_unmount_owned();
        atomic_store(&s_state, cleanup_result == ESP_OK ?
                     FS_STORAGE_UNINITIALIZED : FS_STORAGE_DEINITIALIZING);
        if (cleanup_result != ESP_OK)
        {
            LOG_E("mount rollback failed: %s",
                  esp_err_to_name(cleanup_result));
        }
        goto exit;
    }

    s_data_mounted = true;
    atomic_store(&s_state, FS_STORAGE_READY);
    LOG_I("data partition mounted at %s", FS_DATA_MOUNT_PATH);

exit:
    return result;
}

esp_err_t fs_storage_deinit(void)
{
    esp_err_t result = ESP_OK;
    int state = atomic_load(&s_state);
    if (state == FS_STORAGE_UNINITIALIZED)
    {
        goto exit;
    }
    if (state == FS_STORAGE_READY)
    {
        int expected = FS_STORAGE_READY;
        if (!atomic_compare_exchange_strong(&s_state, &expected,
                                            FS_STORAGE_DEINITIALIZING))
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
    }
    else if (state != FS_STORAGE_DEINITIALIZING)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    result = _fs_storage_unmount_owned();
    if (result == ESP_OK)
    {
        atomic_store(&s_state, FS_STORAGE_UNINITIALIZED);
    }

exit:
    return result;
}

bool fs_storage_is_initialized(void)
{
    return atomic_load(&s_state) == FS_STORAGE_READY;
}

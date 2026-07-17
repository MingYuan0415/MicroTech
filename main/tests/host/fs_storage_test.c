#include "fs_storage/fs_storage.h"

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PARTITION_SIZE ((4096U * 2U) + 317U)

typedef struct fake_storage
{
    esp_err_t res_mount_result;
    esp_err_t res_unmount_result;
    esp_err_t data_mount_result;
    esp_err_t data_mount_retry_result;
    esp_err_t data_unmount_result;
    esp_err_t data_format_result;
    esp_err_t partition_read_result;
    unsigned res_mount_calls;
    unsigned res_unmount_calls;
    unsigned data_mount_calls;
    unsigned data_unmount_calls;
    unsigned data_format_calls;
    unsigned partition_read_calls;
    unsigned heap_alloc_calls;
    unsigned heap_free_calls;
    bool res_format;
    bool data_format;
    bool partition_found;
    bool partition_erased;
    bool heap_allocation_fails;
} fake_storage_t;

static fake_storage_t s_storage;
static const esp_partition_t s_data_partition =
{
    .size = TEST_PARTITION_SIZE,
};

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host-error";
}

void test_log_write(const char *level, const char *tag, const char *format,
                    ...)
{
    (void)level;
    (void)tag;
    (void)format;
}

void *heap_caps_malloc_prefer(size_t size, size_t preference_count, ...)
{
    assert(size == 4096U);
    assert(preference_count == 2U);

    va_list preferences;
    va_start(preferences, preference_count);
    unsigned int first = va_arg(preferences, unsigned int);
    unsigned int second = va_arg(preferences, unsigned int);
    va_end(preferences);
    assert(first == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(second == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    s_storage.heap_alloc_calls++;
    return s_storage.heap_allocation_fails ? NULL : malloc(size);
}

void heap_caps_free(void *memory)
{
    s_storage.heap_free_calls++;
    free(memory);
}

esp_err_t esp_vfs_fat_spiflash_mount_ro(
    const char *base_path, const char *partition_label,
    const esp_vfs_fat_mount_config_t *config)
{
    assert(strcmp(base_path, FS_RES_MOUNT_PATH) == 0);
    assert(strcmp(partition_label, "res") == 0);
    assert(config != NULL);
    assert(config->max_files == 8);
    s_storage.res_mount_calls++;
    s_storage.res_format = config->format_if_mount_failed;
    return s_storage.res_mount_result;
}

esp_err_t esp_vfs_fat_spiflash_unmount_ro(
    const char *base_path, const char *partition_label)
{
    assert(strcmp(base_path, FS_RES_MOUNT_PATH) == 0);
    assert(strcmp(partition_label, "res") == 0);
    s_storage.res_unmount_calls++;
    return s_storage.res_unmount_result;
}

esp_err_t esp_vfs_littlefs_register(
    const esp_vfs_littlefs_conf_t *config)
{
    assert(config != NULL);
    assert(strcmp(config->base_path, FS_DATA_MOUNT_PATH) == 0);
    assert(strcmp(config->partition_label, "data") == 0);
    assert(!config->dont_mount);
    s_storage.data_mount_calls++;
    s_storage.data_format = config->format_if_mount_failed;
    return s_storage.data_mount_calls == 1U ?
           s_storage.data_mount_result : s_storage.data_mount_retry_result;
}

esp_err_t esp_vfs_littlefs_unregister(const char *partition_label)
{
    assert(strcmp(partition_label, "data") == 0);
    s_storage.data_unmount_calls++;
    return s_storage.data_unmount_result;
}

esp_err_t esp_littlefs_format(const char *partition_label)
{
    assert(strcmp(partition_label, "data") == 0);
    s_storage.data_format_calls++;
    return s_storage.data_format_result;
}

const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA);
    assert(subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS);
    assert(strcmp(label, "data") == 0);
    return s_storage.partition_found ? &s_data_partition : NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *destination, size_t size)
{
    assert(partition == &s_data_partition);
    assert(offset + size <= partition->size);
    s_storage.partition_read_calls++;
    if (s_storage.partition_read_result != ESP_OK)
    {
        return s_storage.partition_read_result;
    }
    memset(destination, s_storage.partition_erased ? UINT8_MAX : 0, size);
    return ESP_OK;
}

static void _test_reset_fake(void)
{
    memset(&s_storage, 0, sizeof(s_storage));
    s_storage.data_mount_retry_result = ESP_OK;
    s_storage.partition_found = true;
}

static void _test_happy_path(void)
{
    _test_reset_fake();
    assert(fs_storage_init() == ESP_OK);
    assert(fs_storage_is_initialized());
    assert(!s_storage.data_format);
#if FS_STORAGE_TEST_MMAP
    assert(s_storage.res_mount_calls == 0U);
#else
    assert(s_storage.res_mount_calls == 1U);
    assert(!s_storage.res_format);
#endif
    assert(fs_storage_init() == ESP_OK);
    assert(s_storage.data_mount_calls == 1U);

    assert(fs_storage_deinit() == ESP_OK);
    assert(!fs_storage_is_initialized());
    assert(s_storage.data_unmount_calls == 1U);
#if FS_STORAGE_TEST_MMAP
    assert(s_storage.res_unmount_calls == 0U);
#else
    assert(s_storage.res_unmount_calls == 1U);
#endif
    assert(fs_storage_deinit() == ESP_OK);
}

static void _test_data_mount_rollback(void)
{
    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    assert(fs_storage_init() == ESP_FAIL);
    assert(!fs_storage_is_initialized());
    assert(s_storage.partition_read_calls == 1U);
    assert(s_storage.heap_alloc_calls == 1U);
    assert(s_storage.heap_free_calls == 1U);
    assert(s_storage.data_format_calls == 0U);
#if FS_STORAGE_TEST_MMAP
    assert(s_storage.res_unmount_calls == 0U);
#else
    assert(s_storage.res_unmount_calls == 1U);
#endif

    s_storage.data_mount_result = ESP_OK;
    assert(fs_storage_init() == ESP_OK);
    assert(fs_storage_deinit() == ESP_OK);
}

static void _test_erased_partition_provisioning(void)
{
    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.partition_erased = true;
    assert(fs_storage_init() == ESP_OK);
    assert(fs_storage_is_initialized());
    assert(s_storage.partition_read_calls == 3U);
    assert(s_storage.data_format_calls == 1U);
    assert(s_storage.data_mount_calls == 2U);
    assert(s_storage.heap_alloc_calls == s_storage.heap_free_calls);
    assert(fs_storage_deinit() == ESP_OK);

    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.partition_erased = true;
    s_storage.data_format_result = ESP_FAIL;
    assert(fs_storage_init() == ESP_FAIL);
    assert(s_storage.data_format_calls == 1U);
    assert(fs_storage_deinit() == ESP_OK);

    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.data_mount_retry_result = ESP_FAIL;
    s_storage.partition_erased = true;
    assert(fs_storage_init() == ESP_FAIL);
    assert(s_storage.data_format_calls == 1U);
    assert(s_storage.data_mount_calls == 2U);
    assert(fs_storage_deinit() == ESP_OK);

    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.partition_erased = true;
    s_storage.partition_read_result = ESP_ERR_INVALID_RESPONSE;
    assert(fs_storage_init() == ESP_ERR_INVALID_RESPONSE);
    assert(s_storage.data_format_calls == 0U);
    assert(fs_storage_deinit() == ESP_OK);

    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.partition_found = false;
    assert(fs_storage_init() == ESP_ERR_NOT_FOUND);
    assert(s_storage.data_format_calls == 0U);
    assert(fs_storage_deinit() == ESP_OK);
}

static void _test_probe_allocation_failure(void)
{
    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.heap_allocation_fails = true;
    assert(fs_storage_init() == ESP_ERR_NO_MEM);
    assert(!fs_storage_is_initialized());
    assert(s_storage.heap_alloc_calls == 1U);
    assert(s_storage.heap_free_calls == 1U);
    assert(s_storage.partition_read_calls == 0U);

    s_storage.heap_allocation_fails = false;
    s_storage.data_mount_result = ESP_OK;
    assert(fs_storage_init() == ESP_OK);
    assert(fs_storage_deinit() == ESP_OK);
}

static void _test_deinit_retry(void)
{
    _test_reset_fake();
    assert(fs_storage_init() == ESP_OK);
    s_storage.data_unmount_result = ESP_FAIL;
    assert(fs_storage_deinit() == ESP_FAIL);
    assert(!fs_storage_is_initialized());
    assert(fs_storage_init() == ESP_ERR_INVALID_STATE);

    s_storage.data_unmount_result = ESP_OK;
    assert(fs_storage_deinit() == ESP_OK);
    assert(fs_storage_init() == ESP_OK);

#if !FS_STORAGE_TEST_MMAP
    s_storage.res_unmount_result = ESP_FAIL;
    assert(fs_storage_deinit() == ESP_FAIL);
    assert(fs_storage_init() == ESP_ERR_INVALID_STATE);
    s_storage.res_unmount_result = ESP_OK;
    assert(fs_storage_deinit() == ESP_OK);
#else
    assert(fs_storage_deinit() == ESP_OK);
#endif
}

#if !FS_STORAGE_TEST_MMAP
static void _test_resource_degraded_and_rollback_retry(void)
{
    _test_reset_fake();
    s_storage.res_mount_result = ESP_FAIL;
    assert(fs_storage_init() == ESP_OK);
    assert(fs_storage_deinit() == ESP_OK);
    assert(s_storage.res_unmount_calls == 0U);

    _test_reset_fake();
    s_storage.data_mount_result = ESP_FAIL;
    s_storage.res_unmount_result = ESP_FAIL;
    assert(fs_storage_init() == ESP_FAIL);
    assert(fs_storage_init() == ESP_ERR_INVALID_STATE);
    s_storage.res_unmount_result = ESP_OK;
    assert(fs_storage_deinit() == ESP_OK);

    s_storage.data_mount_result = ESP_OK;
    assert(fs_storage_init() == ESP_OK);
    assert(fs_storage_deinit() == ESP_OK);
}
#endif

int main(void)
{
    _test_happy_path();
    _test_data_mount_rollback();
    _test_erased_partition_provisioning();
    _test_probe_allocation_failure();
    _test_deinit_retry();
#if !FS_STORAGE_TEST_MMAP
    _test_resource_degraded_and_rollback_retry();
#endif
    puts("fs_storage production-source tests passed");
    return 0;
}

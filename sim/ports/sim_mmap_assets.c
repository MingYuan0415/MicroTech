/** @file esp_mmap_assets host port: stage a directory into RAM.
 *
 * The firmware maps the `res` partition read-only; the simulator loads every
 * staged file into resident memory once (the adapter keeps image data
 * pointers that must never be freed early). File ordering and the additive
 * checksum mirror managed_components/espressif__esp_mmap_assets
 * /py_tool/spiffs_assets_gen.py (sort by (extension, basename), name column
 * padded to (longest+1+3)&~3, 0x5A 0x5A prefix before each payload).
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_mmap_assets.h"

typedef struct sim_asset
{
    char *name;
    uint8_t *data;
    size_t size;
} sim_asset_t;

struct mmap_assets_t
{
    sim_asset_t *assets;
    size_t count;
};

#define SIM_ASSET_SKIP_COUNT 3
static struct mmap_assets_t *s_last_instance;

static const char *const s_skip_names[SIM_ASSET_SKIP_COUNT] =
{
    "config.json", "res.bin", "sim_res_meta.h"
};

static const char *s_sim_asset_dir;

void sim_mmap_assets_set_dir(const char *dir)
{
    s_sim_asset_dir = dir;
}

static int _name_cmp(const void *a, const void *b)
{
    const sim_asset_t *left = a;
    const sim_asset_t *right = b;
    const char *la = strrchr(left->name, '.');
    const char *ra = strrchr(right->name, '.');

    if ((la == NULL) || (ra == NULL))
    {
        return strcmp(left->name, right->name);
    }
    const int ext = strcmp(la, ra);
    if (ext != 0)
    {
        return ext;
    }
    const size_t lb = (size_t)(la - left->name);
    const size_t rb = (size_t)(ra - right->name);
    const size_t common = lb < rb ? lb : rb;
    const int base = memcmp(left->name, right->name, common);
    if (base != 0)
    {
        return base;
    }
    return lb < rb ? -1 : (lb > rb);
}

static bool _is_skipped(const char *name)
{
    for (size_t i = 0; i < SIM_ASSET_SKIP_COUNT; i++)
    {
        if (strcmp(name, s_skip_names[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

static esp_err_t _load_file(sim_asset_t *asset, const char *dir,
                            const char *name)
{
    char path[512];
    FILE *fp = NULL;
    struct stat st;
    long size;
    void *data = NULL;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(path, &st) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return ESP_FAIL;
    }
    size = (long)st.st_size;
    data = malloc((size_t)size + 1U);
    if ((data == NULL) ||
            (fread(data, 1, (size_t)size, fp) != (size_t)size))
    {
        (void)fclose(fp);
        free(data);
        return ESP_ERR_NO_MEM;
    }
    (void)fclose(fp);
    asset->name = strdup(name);
    asset->data = data;
    asset->size = (size_t)size;
    return (asset->name != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t _verify_checksum(struct mmap_assets_t *set, uint32_t expect,
                                  uint32_t expect_count)
{
    size_t actual_max = 0;
    size_t name_len;
    size_t running = 0;
    uint32_t sum = 0;

    for (size_t i = 0; i < set->count; i++)
    {
        const size_t len = strlen(set->assets[i].name);
        if (len > actual_max)
        {
            actual_max = len;
        }
    }
    name_len = (actual_max + 1U + 3U) & ~(size_t)3U;
    for (size_t i = 0; i < set->count; i++)
    {
        const sim_asset_t *asset = &set->assets[i];
        const size_t name_bytes = strlen(asset->name);
        uint32_t fields[4];
        fields[0] = (uint32_t)asset->size;
        fields[1] = (uint32_t)running;
        fields[2] = 0U;
        fields[3] = 0U;
        for (size_t n = 0; n < name_len; n++)
        {
            sum += (n < name_bytes) ? (uint8_t)asset->name[n] : 0U;
        }
        for (size_t f = 0; f < 4U; f++)
        {
            sum += (uint8_t)(fields[f] & 0xFFU);
            sum += (uint8_t)((fields[f] >> 8) & 0xFFU);
            sum += (uint8_t)((fields[f] >> 16) & 0xFFU);
            sum += (uint8_t)((fields[f] >> 24) & 0xFFU);
        }
        sum += 0x5AU;
        sum += 0x5AU;
        for (size_t b = 0; b < asset->size; b++)
        {
            sum += asset->data[b];
        }
        running += asset->size + 2U;
    }
    if ((set->count != expect_count) ||
            ((sum & 0xFFFFU) != (expect & 0xFFFFU)))
    {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t mmap_assets_new(const mmap_assets_config_t *config,
                          mmap_assets_handle_t *ret_item)
{
    struct mmap_assets_t *set = NULL;
    sim_asset_t *assets = NULL;
    size_t capacity = 0;
    size_t count = 0;
    struct dirent *entry;
    DIR *dir = NULL;

    if ((config == NULL) || (ret_item == NULL) || (s_sim_asset_dir == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    dir = opendir(s_sim_asset_dir);
    if (dir == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    for (;;)
    {
        entry = readdir(dir);
        if (entry == NULL)
        {
            break;
        }
        if ((entry->d_name[0] == '.') || _is_skipped(entry->d_name))
        {
            continue;
        }
        if (count == capacity)
        {
            sim_asset_t *grown;
            capacity = (capacity == 0U) ? 32U : capacity * 2U;
            grown = realloc(assets, capacity * sizeof(*assets));
            if (grown == NULL)
            {
                goto fail;
            }
            assets = grown;
        }
        if (_load_file(&assets[count], s_sim_asset_dir, entry->d_name) !=
                ESP_OK)
        {
            goto fail;
        }
        count++;
    }
    (void)closedir(dir);
    dir = NULL;
    qsort(assets, count, sizeof(*assets), _name_cmp);

    set = calloc(1, sizeof(*set));
    if (set == NULL)
    {
        goto fail;
    }
    set->assets = assets;
    set->count = count;
    if (_verify_checksum(set, config->checksum,
                         (uint32_t)config->max_files) != ESP_OK)
    {
        goto fail;
    }
    *ret_item = set;
    s_last_instance = set;
    return ESP_OK;

fail:
    if (dir != NULL)
    {
        (void)closedir(dir);
    }
    for (size_t i = 0; i < count; i++)
    {
        free(assets[i].name);
        free(assets[i].data);
    }
    free(assets);
    free(set);
    return ESP_ERR_INVALID_CRC;
}

esp_err_t mmap_assets_del(mmap_assets_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_last_instance == handle)
    {
        s_last_instance = NULL;
    }
    for (size_t i = 0; i < handle->count; i++)
    {
        free(handle->assets[i].name);
        free(handle->assets[i].data);
    }
    free(handle->assets);
    free(handle);
    return ESP_OK;
}

int mmap_assets_get_stored_files(mmap_assets_handle_t handle)
{
    return (handle == NULL) ? 0 : (int)handle->count;
}

const uint8_t *mmap_assets_get_mem(mmap_assets_handle_t handle, int index)
{
    if ((handle == NULL) || (index < 0) ||
            ((size_t)index >= handle->count))
    {
        return NULL;
    }
    return handle->assets[index].data;
}

int mmap_assets_get_size(mmap_assets_handle_t handle, int index)
{
    if ((handle == NULL) || (index < 0) ||
            ((size_t)index >= handle->count))
    {
        return -1;
    }
    return (int)handle->assets[index].size;
}

const uint8_t *sim_mmap_assets_blob(const char *name, size_t *out_size)
{
    if ((s_last_instance == NULL) || (name == NULL))
    {
        return NULL;
    }
    for (size_t i = 0; i < s_last_instance->count; i++)
    {
        if (strcmp(s_last_instance->assets[i].name, name) == 0)
        {
            *out_size = s_last_instance->assets[i].size;
            return s_last_instance->assets[i].data;
        }
    }
    return NULL;
}

const char *mmap_assets_get_name(mmap_assets_handle_t handle, int index)
{
    if ((handle == NULL) || (index < 0) ||
            ((size_t)index >= handle->count))
    {
        return NULL;
    }
    return handle->assets[index].name;
}

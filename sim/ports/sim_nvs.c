/** @file NVS file backend: one small record file per namespace.
 *
 * Persistence contract matches what nv_storage/factory_reset/onboarding
 * expect: open namespace -> cached records -> commit flushes atomically
 * (temp file + rename), so a simulator restart re-reads the same store.
 * Handles that share a namespace share one store under a global lock.
 */
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nvs.h"
#include "nvs_flash.h"

#define SIM_NVS_MAX_HANDLES 16
#define SIM_NVS_TYPE_U8     1U
#define SIM_NVS_TYPE_U16    2U
#define SIM_NVS_TYPE_U32    3U
#define SIM_NVS_TYPE_STR    4U
#define SIM_NVS_TYPE_BLOB   5U

typedef struct nvs_record
{
    char *key;
    uint8_t type;
    uint8_t *data;
    size_t size;
    struct nvs_record *next;
} nvs_record_t;

typedef struct nvs_store
{
    bool occupied;
    unsigned refs;
    char namespace_name[NVS_KEY_NAME_MAX_SIZE];
    nvs_record_t *records;
} nvs_store_t;

typedef struct nvs_slot
{
    bool in_use;
    nvs_store_t *store;
} nvs_slot_t;

static nvs_store_t s_stores[SIM_NVS_MAX_HANDLES];
static nvs_slot_t s_slots[SIM_NVS_MAX_HANDLES];
static const char *s_nvs_dir = "sim_nvs";
static pthread_mutex_t s_nvs_lock = PTHREAD_MUTEX_INITIALIZER;

void sim_nvs_set_dir(const char *dir);

static void _trace(const char *op, const char *key, esp_err_t result);

void sim_nvs_set_dir(const char *dir)
{
    if (dir != NULL)
    {
        (void)pthread_mutex_lock(&s_nvs_lock);
        s_nvs_dir = dir;
        (void)pthread_mutex_unlock(&s_nvs_lock);
    }
}

static nvs_slot_t *_slot(nvs_handle_t handle)
{
    if ((handle == 0U) || (handle > SIM_NVS_MAX_HANDLES))
    {
        return NULL;
    }
    nvs_slot_t *slot = &s_slots[handle - 1U];
    return (slot->in_use && (slot->store != NULL)) ? slot : NULL;
}

static nvs_record_t *_find(nvs_store_t *store, const char *key)
{
    for (nvs_record_t *r = store->records; r != NULL; r = r->next)
    {
        if (strcmp(r->key, key) == 0)
        {
            return r;
        }
    }
    return NULL;
}

static void _path(char *out, size_t out_size, const char *ns, bool temp)
{
    snprintf(out, out_size, "%s/%s%s", s_nvs_dir, ns,
             temp ? ".tmp" : ".nvs");
}

static void _records_clear(nvs_record_t *records)
{
    while (records != NULL)
    {
        nvs_record_t *next = records->next;
        free(records->key);
        free(records->data);
        free(records);
        records = next;
    }
}

static void _store_reset(nvs_store_t *store)
{
    _records_clear(store->records);
    store->records = NULL;
    store->refs = 0U;
    store->occupied = false;
    store->namespace_name[0] = '\0';
}

static esp_err_t _load(nvs_store_t *store)
{
    char path[256];
    FILE *fp;
    uint32_t count;

    _path(path, sizeof(path), store->namespace_name, false);
    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return ESP_OK;
    }
    if (fread(&count, sizeof(count), 1U, fp) != 1U)
    {
        (void)fclose(fp);
        return ESP_FAIL;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t type;
        uint8_t klen;
        uint32_t dlen;
        char key[256];
        nvs_record_t *rec;

        if ((fread(&type, 1U, 1U, fp) != 1U) ||
                (fread(&klen, 1U, 1U, fp) != 1U) ||
                (fread(key, 1U, klen, fp) != klen) ||
                (fread(&dlen, sizeof(dlen), 1U, fp) != 1U))
        {
            (void)fclose(fp);
            _records_clear(store->records);
            store->records = NULL;
            return ESP_FAIL;
        }
        key[klen] = '\0';
        rec = calloc(1, sizeof(*rec));
        if (rec == NULL)
        {
            (void)fclose(fp);
            return ESP_ERR_NO_MEM;
        }
        rec->key = strdup(key);
        rec->type = type;
        rec->size = dlen;
        rec->data = (dlen > 0U) ? malloc(dlen) : NULL;
        if ((rec->key == NULL) || ((dlen > 0U) &&
                                   (fread(rec->data, 1U, dlen, fp) != dlen)))
        {
            free(rec->key);
            free(rec->data);
            free(rec);
            (void)fclose(fp);
            return ESP_FAIL;
        }
        rec->next = store->records;
        store->records = rec;
    }
    (void)fclose(fp);
    return ESP_OK;
}

static esp_err_t _store_flush(nvs_store_t *store)
{
    char path[256];
    char temp[256];
    FILE *fp;
    uint32_t count = 0;

    (void)mkdir(s_nvs_dir, 0775);
    for (nvs_record_t *r = store->records; r != NULL; r = r->next)
    {
        count++;
    }
    _path(temp, sizeof(temp), store->namespace_name, true);
    fp = fopen(temp, "wb");
    if (fp == NULL)
    {
        return ESP_FAIL;
    }
    if (fwrite(&count, sizeof(count), 1U, fp) != 1U)
    {
        (void)fclose(fp);
        remove(temp);
        return ESP_FAIL;
    }
    for (nvs_record_t *r = store->records; r != NULL; r = r->next)
    {
        uint8_t klen = (uint8_t)strlen(r->key);
        uint32_t dlen = (uint32_t)r->size;
        if ((fwrite(&r->type, 1U, 1U, fp) != 1U) ||
                (fwrite(&klen, 1U, 1U, fp) != 1U) ||
                (fwrite(r->key, 1U, klen, fp) != klen) ||
                (fwrite(&dlen, sizeof(dlen), 1U, fp) != 1U) ||
                ((dlen > 0U) && (fwrite(r->data, 1U, dlen, fp) != dlen)))
        {
            (void)fclose(fp);
            remove(temp);
            return ESP_FAIL;
        }
    }
    (void)fclose(fp);
    _trace("commit", store->namespace_name, ESP_OK);
    _path(path, sizeof(path), store->namespace_name, false);
    if (rename(temp, path) != 0)
    {
        remove(temp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t _set(nvs_handle_t handle, const char *key, uint8_t type,
                      const void *data, size_t size)
{
    nvs_slot_t *slot;
    nvs_store_t *store;
    nvs_record_t *rec;

    (void)pthread_mutex_lock(&s_nvs_lock);
    slot = _slot(handle);
    if ((slot == NULL) || (key == NULL) || (data == NULL) ||
            (strlen(key) >= NVS_KEY_NAME_MAX_SIZE))
    {
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_INVALID_ARG;
    }
    store = slot->store;
    rec = _find(store, key);
    if (rec == NULL)
    {
        rec = calloc(1, sizeof(*rec));
        if (rec == NULL)
        {
            (void)pthread_mutex_unlock(&s_nvs_lock);
            return ESP_ERR_NO_MEM;
        }
        rec->key = strdup(key);
        if (rec->key == NULL)
        {
            free(rec);
            (void)pthread_mutex_unlock(&s_nvs_lock);
            return ESP_ERR_NO_MEM;
        }
        rec->next = store->records;
        store->records = rec;
    }
    else
    {
        free(rec->data);
    }
    rec->data = malloc(size);
    if (rec->data == NULL)
    {
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_NO_MEM;
    }
    memcpy(rec->data, data, size);
    rec->type = type;
    rec->size = size;
    _trace("set", key, ESP_OK);
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return ESP_OK;
}

static esp_err_t _get(nvs_handle_t handle, const char *key, uint8_t type,
                      void *out, size_t *size)
{
    nvs_slot_t *slot;
    nvs_record_t *rec;
    esp_err_t result = ESP_OK;

    (void)pthread_mutex_lock(&s_nvs_lock);
    slot = _slot(handle);
    if ((slot == NULL) || (key == NULL))
    {
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_INVALID_ARG;
    }
    rec = _find(slot->store, key);
    if (rec == NULL)
    {
        _trace("get", key, ESP_ERR_NVS_NOT_FOUND);
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    else if (rec->type != type)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    else if (out == NULL)
    {
        if (size != NULL)
        {
            *size = rec->size;
        }
    }
    else if ((size == NULL) || (*size < rec->size))
    {
        result = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        memcpy(out, rec->data, rec->size);
        *size = rec->size;
        _trace("get", key, ESP_OK);
    }
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return result;
}

static void _trace(const char *op, const char *key, esp_err_t result)
{
    if (getenv("SIM_NVS_TRACE") != NULL)
    {
        fprintf(stderr, "sim_nvs: %s %s -> %s\n", op, key ? key : "-",
                esp_err_to_name(result));
    }
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    nvs_store_t *store = NULL;
    nvs_slot_t *slot = NULL;
    nvs_handle_t handle = 0U;

    (void)open_mode;
    if ((namespace_name == NULL) || (out_handle == NULL) ||
            (strlen(namespace_name) >= NVS_KEY_NAME_MAX_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_nvs_lock);
    for (size_t i = 0; i < SIM_NVS_MAX_HANDLES; i++)
    {
        if (s_stores[i].occupied &&
                (strcmp(s_stores[i].namespace_name, namespace_name) == 0))
        {
            store = &s_stores[i];
            break;
        }
    }
    if (store == NULL)
    {
        for (size_t i = 0; i < SIM_NVS_MAX_HANDLES; i++)
        {
            if (!s_stores[i].occupied)
            {
                store = &s_stores[i];
                store->occupied = true;
                store->refs = 0U;
                store->records = NULL;
                snprintf(store->namespace_name, sizeof(store->namespace_name),
                         "%s", namespace_name);
                if (_load(store) != ESP_OK)
                {
                    _store_reset(store);
                    (void)pthread_mutex_unlock(&s_nvs_lock);
                    return ESP_FAIL;
                }
                break;
            }
        }
    }
    if (store == NULL)
    {
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_NO_MEM;
    }
    for (nvs_handle_t i = 1U; i <= SIM_NVS_MAX_HANDLES; i++)
    {
        if (!s_slots[i - 1U].in_use)
        {
            slot = &s_slots[i - 1U];
            handle = i;
            break;
        }
    }
    if (slot == NULL)
    {
        if (store->refs == 0U)
        {
            _store_reset(store);
        }
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_NO_MEM;
    }
    slot->in_use = true;
    slot->store = store;
    store->refs++;
    *out_handle = handle;
    _trace("open", namespace_name, ESP_OK);
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)pthread_mutex_lock(&s_nvs_lock);
    nvs_slot_t *slot = _slot(handle);
    if (slot != NULL)
    {
        nvs_store_t *store = slot->store;
        slot->in_use = false;
        slot->store = NULL;
        if (store->refs > 0U)
        {
            store->refs--;
        }
        if (store->refs == 0U)
        {
            _store_reset(store);
        }
    }
    (void)pthread_mutex_unlock(&s_nvs_lock);
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    esp_err_t result;

    (void)pthread_mutex_lock(&s_nvs_lock);
    nvs_slot_t *slot = _slot(handle);
    result = (slot == NULL) ? ESP_ERR_INVALID_ARG : _store_flush(slot->store);
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return result;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return _set(handle, key, SIM_NVS_TYPE_U8, &value, sizeof(value));
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *output)
{
    size_t size = output ? sizeof(*output) : 0U;
    return _get(handle, key, SIM_NVS_TYPE_U8, output, &size);
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    return _set(handle, key, SIM_NVS_TYPE_U16, &value, sizeof(value));
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *output)
{
    size_t size = output ? sizeof(*output) : 0U;
    return _get(handle, key, SIM_NVS_TYPE_U16, output, &size);
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return _set(handle, key, SIM_NVS_TYPE_U32, &value, sizeof(value));
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *output)
{
    size_t size = output ? sizeof(*output) : 0U;
    return _get(handle, key, SIM_NVS_TYPE_U32, output, &size);
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    return _set(handle, key, SIM_NVS_TYPE_STR, value, strlen(value) + 1U);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *output,
                      size_t *length)
{
    return _get(handle, key, SIM_NVS_TYPE_STR, output, length);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    return _set(handle, key, SIM_NVS_TYPE_BLOB, value, length);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output,
                       size_t *length)
{
    return _get(handle, key, SIM_NVS_TYPE_BLOB, output, length);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    esp_err_t result = ESP_ERR_NVS_NOT_FOUND;

    (void)pthread_mutex_lock(&s_nvs_lock);
    nvs_slot_t *slot = _slot(handle);
    if ((slot == NULL) || (key == NULL))
    {
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_record_t *prev = NULL;
    for (nvs_record_t *r = slot->store->records; r != NULL; prev = r, r = r->next)
    {
        if (strcmp(r->key, key) == 0)
        {
            if (prev == NULL)
            {
                slot->store->records = r->next;
            }
            else
            {
                prev->next = r->next;
            }
            free(r->key);
            free(r->data);
            free(r);
            result = ESP_OK;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return result;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    (void)pthread_mutex_lock(&s_nvs_lock);
    nvs_slot_t *slot = _slot(handle);
    if (slot == NULL)
    {
        (void)pthread_mutex_unlock(&s_nvs_lock);
        return ESP_ERR_INVALID_ARG;
    }
    _records_clear(slot->store->records);
    slot->store->records = NULL;
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return ESP_OK;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_deinit(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    DIR *dir;

    (void)pthread_mutex_lock(&s_nvs_lock);
    for (size_t i = 0; i < SIM_NVS_MAX_HANDLES; i++)
    {
        s_slots[i].in_use = false;
        s_slots[i].store = NULL;
        _store_reset(&s_stores[i]);
    }
    dir = opendir(s_nvs_dir);
    if (dir != NULL)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            const size_t len = strlen(entry->d_name);
            if ((len > 4U) &&
                    ((strcmp(entry->d_name + len - 4U, ".nvs") == 0) ||
                     (strcmp(entry->d_name + len - 4U, ".tmp") == 0)))
            {
                char path[512];
                if (snprintf(path, sizeof(path), "%s/%s",
                             s_nvs_dir, entry->d_name) >= (int)sizeof(path))
                {
                    continue;
                }
                (void)unlink(path);
            }
        }
        (void)closedir(dir);
    }
    (void)pthread_mutex_unlock(&s_nvs_lock);
    return ESP_OK;
}

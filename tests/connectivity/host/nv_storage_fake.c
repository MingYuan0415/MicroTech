#include "host_nv_storage.h"

#include "nv_storage.h"

#include <pthread.h>
#include <string.h>

#define HOST_NV_STORAGE_CAPACITY 256U

typedef struct host_nv_storage_state
{
    pthread_mutex_t lock;
    uint8_t data[HOST_NV_STORAGE_CAPACITY];
    size_t size;
    bool present;
    esp_err_t next_get;
    esp_err_t next_set;
    esp_err_t next_erase;
    unsigned set_count;
    unsigned erase_count;
} host_nv_storage_state_t;

static host_nv_storage_state_t s_storage =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

void host_nv_storage_reset(void)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    memset(s_storage.data, 0, sizeof(s_storage.data));
    s_storage.size = 0U;
    s_storage.present = false;
    s_storage.next_get = ESP_OK;
    s_storage.next_set = ESP_OK;
    s_storage.next_erase = ESP_OK;
    s_storage.set_count = 0U;
    s_storage.erase_count = 0U;
    (void)pthread_mutex_unlock(&s_storage.lock);
}

void host_nv_storage_fail_next_get(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    s_storage.next_get = result;
    (void)pthread_mutex_unlock(&s_storage.lock);
}

void host_nv_storage_fail_next_set(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    s_storage.next_set = result;
    (void)pthread_mutex_unlock(&s_storage.lock);
}

void host_nv_storage_fail_next_erase(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    s_storage.next_erase = result;
    (void)pthread_mutex_unlock(&s_storage.lock);
}

void host_nv_storage_seed(const void *data, size_t size)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    memset(s_storage.data, 0, sizeof(s_storage.data));
    if (data != NULL && size <= sizeof(s_storage.data))
    {
        memcpy(s_storage.data, data, size);
        s_storage.size = size;
        s_storage.present = true;
    }
    else
    {
        s_storage.size = 0U;
        s_storage.present = false;
    }
    (void)pthread_mutex_unlock(&s_storage.lock);
}

bool host_nv_storage_copy(void *data, size_t capacity, size_t *size)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    const bool present = s_storage.present;
    if (size != NULL)
    {
        *size = s_storage.size;
    }
    if (present && data != NULL && capacity >= s_storage.size)
    {
        memcpy(data, s_storage.data, s_storage.size);
    }
    (void)pthread_mutex_unlock(&s_storage.lock);
    return present;
}

unsigned host_nv_storage_set_count(void)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    const unsigned count = s_storage.set_count;
    (void)pthread_mutex_unlock(&s_storage.lock);
    return count;
}

unsigned host_nv_storage_erase_count(void)
{
    (void)pthread_mutex_lock(&s_storage.lock);
    const unsigned count = s_storage.erase_count;
    (void)pthread_mutex_unlock(&s_storage.lock);
    return count;
}

esp_err_t nv_storage_get_blob(const char *key, void *output, size_t *size)
{
    if (key == NULL || size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_storage.lock);
    esp_err_t result = s_storage.next_get;
    s_storage.next_get = ESP_OK;
    if (result == ESP_OK && !s_storage.present)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    if (result == ESP_OK && *size < s_storage.size)
    {
        *size = s_storage.size;
        result = ESP_ERR_INVALID_SIZE;
    }
    if (result == ESP_OK)
    {
        memcpy(output, s_storage.data, s_storage.size);
        *size = s_storage.size;
    }
    (void)pthread_mutex_unlock(&s_storage.lock);
    return result;
}

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t size)
{
    if (key == NULL || data == NULL || size > sizeof(s_storage.data))
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_storage.lock);
    const esp_err_t result = s_storage.next_set;
    s_storage.next_set = ESP_OK;
    if (result == ESP_OK)
    {
        memset(s_storage.data, 0, sizeof(s_storage.data));
        memcpy(s_storage.data, data, size);
        s_storage.size = size;
        s_storage.present = true;
        ++s_storage.set_count;
    }
    (void)pthread_mutex_unlock(&s_storage.lock);
    return result;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_storage.lock);
    esp_err_t result = s_storage.next_erase;
    s_storage.next_erase = ESP_OK;
    if (result == ESP_OK && !s_storage.present)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    if (result == ESP_OK)
    {
        memset(s_storage.data, 0, sizeof(s_storage.data));
        s_storage.size = 0U;
        s_storage.present = false;
        ++s_storage.erase_count;
    }
    (void)pthread_mutex_unlock(&s_storage.lock);
    return result;
}

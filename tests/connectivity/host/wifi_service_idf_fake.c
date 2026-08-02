#include "host_wifi_idf.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "freertos/task.h"
#include "wifi_service.h"
#include "wifi_service_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HOST_EVENT_HANDLER_CAPACITY 4U
#define HOST_SCAN_RECORD_CAPACITY   16U

typedef struct host_event_handler
{
    bool used;
    esp_event_base_t event_base;
    int32_t event_id;
    esp_event_handler_t handler;
    void *argument;
} host_event_handler_t;

static host_event_handler_t s_handlers[HOST_EVENT_HANDLER_CAPACITY];
static esp_netif_t s_netif;
static bool s_netif_owned;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_init_nvs_enabled;
static unsigned s_sequence;
static unsigned s_init_sequence;
static unsigned s_storage_sequence;
static unsigned s_mode_sequence;
static wifi_storage_t s_storage;
static TickType_t s_ticks;
static bool s_submitted_event_valid;
static wifi_service_port_event_t s_submitted_event;
static wifi_ap_record_t s_scan_records[HOST_SCAN_RECORD_CAPACITY];
static size_t s_scan_record_count;
static size_t s_scan_record_index;
static unsigned s_scan_clear_count;

esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t IP_EVENT = "IP_EVENT";

void host_wifi_idf_reset(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    memset(&s_netif, 0, sizeof(s_netif));
    s_netif_owned = false;
    s_wifi_initialized = false;
    s_wifi_started = false;
    s_init_nvs_enabled = false;
    s_sequence = 0U;
    s_init_sequence = 0U;
    s_storage_sequence = 0U;
    s_mode_sequence = 0U;
    s_storage = WIFI_STORAGE_FLASH;
    s_ticks = 0U;
    s_submitted_event_valid = false;
    memset(&s_submitted_event, 0, sizeof(s_submitted_event));
    memset(s_scan_records, 0, sizeof(s_scan_records));
    s_scan_record_count = 0U;
    s_scan_record_index = 0U;
    s_scan_clear_count = 0U;
}

bool host_wifi_idf_init_nvs_enabled(void)
{
    return s_init_nvs_enabled;
}

unsigned host_wifi_idf_init_sequence(void)
{
    return s_init_sequence;
}

unsigned host_wifi_idf_storage_sequence(void)
{
    return s_storage_sequence;
}

wifi_storage_t host_wifi_idf_storage(void)
{
    return s_storage;
}

unsigned host_wifi_idf_mode_sequence(void)
{
    return s_mode_sequence;
}

bool host_wifi_idf_emit_disconnect(uint16_t reason,
                                   wifi_service_port_event_t *event)
{
    wifi_event_sta_disconnected_t disconnected =
    {
        .reason = reason,
    };
    s_submitted_event_valid = false;
    memset(&s_submitted_event, 0, sizeof(s_submitted_event));
    if (esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                       &disconnected, sizeof(disconnected), 0U) != ESP_OK ||
            !s_submitted_event_valid)
    {
        return false;
    }
    if (event != NULL)
    {
        *event = s_submitted_event;
    }
    return true;
}

void host_wifi_idf_set_scan_records(const wifi_ap_record_t *records,
                                    size_t count)
{
    if (count > HOST_SCAN_RECORD_CAPACITY)
    {
        count = HOST_SCAN_RECORD_CAPACITY;
    }
    memset(s_scan_records, 0, sizeof(s_scan_records));
    if (records != NULL && count > 0U)
    {
        memcpy(s_scan_records, records, count * sizeof(*records));
    }
    s_scan_record_count = count;
    s_scan_record_index = 0U;
}

unsigned host_wifi_idf_scan_clear_count(void)
{
    return s_scan_clear_count;
}

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t handler, void *argument,
    esp_event_handler_instance_t *instance)
{
    if (event_base == NULL || handler == NULL || instance == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < HOST_EVENT_HANDLER_CAPACITY; ++index)
    {
        if (!s_handlers[index].used)
        {
            s_handlers[index] = (host_event_handler_t)
            {
                .used = true,
                .event_base = event_base,
                .event_id = event_id,
                .handler = handler,
                .argument = argument,
            };
            *instance = &s_handlers[index];
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_instance_t instance)
{
    host_event_handler_t *entry = instance;
    if (entry == NULL || !entry->used || entry->event_base != event_base ||
            entry->event_id != event_id)
    {
        return ESP_ERR_NOT_FOUND;
    }
    memset(entry, 0, sizeof(*entry));
    return ESP_OK;
}

esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id,
                         const void *event_data, size_t event_data_size,
                         uint32_t ticks_to_wait)
{
    (void)event_data_size;
    (void)ticks_to_wait;
    for (size_t index = 0U; index < HOST_EVENT_HANDLER_CAPACITY; ++index)
    {
        host_event_handler_t *entry = &s_handlers[index];
        if (entry->used && entry->event_base == event_base &&
                (entry->event_id == event_id ||
                 entry->event_id == ESP_EVENT_ANY_ID))
        {
            entry->handler(entry->argument, event_base, event_id,
                           (void *)event_data);
        }
    }
    return ESP_OK;
}

esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    if (config == NULL || s_netif_owned)
    {
        return NULL;
    }
    s_netif.marker = config->marker;
    s_netif_owned = true;
    return &s_netif;
}

void esp_netif_destroy(esp_netif_t *netif)
{
    if (netif == &s_netif)
    {
        s_netif_owned = false;
    }
}

esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif)
{
    return netif == &s_netif && s_netif_owned ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_wifi_set_default_wifi_sta_handlers(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *netif)
{
    return netif == &s_netif ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_init_nvs_enabled = config->nvs_enable;
    s_init_sequence = ++s_sequence;
    s_wifi_initialized = true;
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    if (!s_wifi_initialized)
    {
        return ESP_ERR_WIFI_NOT_INIT;
    }
    s_wifi_initialized = false;
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    if (!s_wifi_initialized)
    {
        return ESP_ERR_WIFI_NOT_INIT;
    }
    s_storage = storage;
    s_storage_sequence = ++s_sequence;
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    if (!s_wifi_initialized || mode != WIFI_MODE_STA)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_mode_sequence = ++s_sequence;
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    if (!s_wifi_initialized)
    {
        return ESP_ERR_WIFI_NOT_INIT;
    }
    s_wifi_started = true;
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    if (!s_wifi_initialized)
    {
        return ESP_ERR_WIFI_NOT_INIT;
    }
    s_wifi_started = false;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    (void)block;
    s_scan_record_index = 0U;
    return s_wifi_started && config != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_wifi_scan_stop(void)
{
    return s_wifi_started ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED;
}

esp_err_t esp_wifi_clear_ap_list(void)
{
    ++s_scan_clear_count;
    s_scan_record_index = s_scan_record_count;
    return s_wifi_initialized ? ESP_OK : ESP_ERR_WIFI_NOT_INIT;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number)
{
    if (number == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *number = (uint16_t)s_scan_record_count;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *record)
{
    if (record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_scan_record_index >= s_scan_record_count)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *record = s_scan_records[s_scan_record_index];
    ++s_scan_record_index;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number,
                                       wifi_ap_record_t *records)
{
    if (number == NULL || records == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *number = 0U;
    return ESP_OK;
}

esp_err_t esp_wifi_set_config(wifi_interface_t interface,
                              const wifi_config_t *config)
{
    return s_wifi_initialized && interface == WIFI_IF_STA && config != NULL ?
               ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_wifi_connect(void)
{
    return s_wifi_started ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED;
}

esp_err_t esp_wifi_disconnect(void)
{
    return s_wifi_started ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED;
}

esp_err_t wifi_service_port_submit_event(
    const wifi_service_port_event_t *event)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_submitted_event = *event;
    s_submitted_event_valid = true;
    return ESP_OK;
}

void wifi_service_secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (bytes != NULL && size > 0U)
    {
        *bytes = 0U;
        ++bytes;
        --size;
    }
}

TickType_t xTaskGetTickCount(void)
{
    return s_ticks;
}

void vTaskDelay(TickType_t ticks)
{
    s_ticks += ticks;
}

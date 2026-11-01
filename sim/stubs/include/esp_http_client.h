#ifndef __WEATHER_HOST_ESP_HTTP_CLIENT_H__
#define __WEATHER_HOST_ESP_HTTP_CLIENT_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct weather_host_http_client *esp_http_client_handle_t;

/** @brief HTTP method used by the music-service host port. */
typedef enum esp_http_client_method
{
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_PATCH,
} esp_http_client_method_t;

typedef enum esp_http_client_event_id
{
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_HEADERS_COMPLETE,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event
{
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *event);

typedef struct esp_http_client_config
{
    const char *url;
    esp_http_client_method_t method;
    http_event_handle_cb event_handler;
    void *user_data;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *config);
    bool disable_auto_redirect;
    int buffer_size;
    int buffer_size_tx;
    bool keep_alive_enable;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
        const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cancel_request(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client,
                               int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer,
                         int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif /* __WEATHER_HOST_ESP_HTTP_CLIENT_H__ */

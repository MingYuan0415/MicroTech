/** @file esp_http_client host port on libcurl (weather fetch path).
 *
 * Faithfully replays the IDF event sequence the weather port consumes:
 * ON_CONNECTED -> per-header ON_HEADER -> ON_HEADERS_COMPLETE -> chunked
 * ON_DATA (handler return gates the transfer) -> ON_FINISH/DISCONNECTED.
 * cancel_request flips a flag observed by the write callback, matching the
 * IDF asynchronous cancel semantics.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "esp_http_client.h"

#define SIM_HTTP_SCRIPT_MAX 6
typedef struct sim_http_script
{
    char *match;
    int status;
    char *content_type;
    uint8_t *body;
    size_t size;
} sim_http_script_t;

static atomic_bool s_script_only;
static sim_http_script_t s_scripts[SIM_HTTP_SCRIPT_MAX];
static size_t s_script_count;
static pthread_mutex_t s_script_lock = PTHREAD_MUTEX_INITIALIZER;

int sim_http_set_response(const char *url_substring, int status,
                          const char *content_type,
                          const uint8_t *body, size_t size);

int sim_http_set_script_only(bool enabled);

int sim_http_set_script_only(bool enabled)
{
    atomic_store(&s_script_only, enabled);
    return 0;
}

int sim_http_set_response(const char *url_substring, int status,
                          const char *content_type,
                          const uint8_t *body, size_t size)
{
    if (url_substring == NULL)
    {
        return -1;
    }
    (void)pthread_mutex_lock(&s_script_lock);
    for (size_t i = 0; i < s_script_count; i++)
    {
        if (strcmp(s_scripts[i].match, url_substring) == 0)
        {
            free(s_scripts[i].body);
            s_scripts[i].body = (body != NULL && size > 0U)
                                ? (uint8_t *)malloc(size) : NULL;
            if (s_scripts[i].body != NULL)
            {
                memcpy(s_scripts[i].body, body, size);
            }
            s_scripts[i].size = (s_scripts[i].body != NULL) ? size : 0U;
            s_scripts[i].status = status;
            free(s_scripts[i].content_type);
            s_scripts[i].content_type =
                (content_type != NULL) ? strdup(content_type) : NULL;
            (void)pthread_mutex_unlock(&s_script_lock);
            return 0;
        }
    }
    if (s_script_count >= SIM_HTTP_SCRIPT_MAX)
    {
        (void)pthread_mutex_unlock(&s_script_lock);
        return -1;
    }
    sim_http_script_t *slot = &s_scripts[s_script_count++];
    slot->match = strdup(url_substring);
    slot->content_type = (content_type != NULL) ? strdup(content_type) : NULL;
    slot->size = (body != NULL && size > 0U) ? size : 0U;
    slot->body = (slot->size > 0U) ? malloc(slot->size) : NULL;
    if (slot->body != NULL)
    {
        memcpy(slot->body, body, slot->size);
    }
    slot->status = status;
    (void)pthread_mutex_unlock(&s_script_lock);
    return (slot->match != NULL) ? 0 : -1;
}

static void _script_release(sim_http_script_t *script)
{
    free(script->match);
    free(script->content_type);
    free(script->body);
    memset(script, 0, sizeof(*script));
}

static bool _script_snapshot(const char *url, sim_http_script_t *out)
{
    memset(out, 0, sizeof(*out));
    if (url == NULL)
    {
        return false;
    }
    (void)pthread_mutex_lock(&s_script_lock);
    for (size_t i = 0; i < s_script_count; i++)
    {
        if (strstr(url, s_scripts[i].match) == NULL)
        {
            continue;
        }
        out->status = s_scripts[i].status;
        out->size = s_scripts[i].size;
        if (s_scripts[i].content_type != NULL)
        {
            out->content_type = strdup(s_scripts[i].content_type);
        }
        if ((s_scripts[i].body != NULL) && (s_scripts[i].size > 0U))
        {
            out->body = malloc(s_scripts[i].size);
            if (out->body != NULL)
            {
                memcpy(out->body, s_scripts[i].body, s_scripts[i].size);
            }
            else
            {
                out->size = 0U;
            }
        }
        (void)pthread_mutex_unlock(&s_script_lock);
        return true;
    }
    (void)pthread_mutex_unlock(&s_script_lock);
    return false;
}

struct weather_host_http_client
{
    char *url;
    http_event_handle_cb handler;
    void *user_data;
    long timeout_ms;
    bool disable_auto_redirect;
    char **headers;
    size_t header_count;
    atomic_bool cancel;
    int status_code;
};

static esp_err_t _emit(esp_http_client_handle_t client,
                       esp_http_client_event_id_t id, void *data, int len,
                       const char *key, const char *value)
{
    esp_http_client_event_t event =
    {
        .event_id = id,
        .client = client,
        .data = data,
        .data_len = len,
        .user_data = client->user_data,
        .header_key = (char *)key,
        .header_value = (char *)value,
    };

    if (client->handler == NULL)
    {
        return ESP_OK;
    }
    return client->handler(&event);
}

esp_err_t esp_crt_bundle_attach(void *config)
{
    (void)config;
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t *config)
{
    if (config == NULL || config->url == NULL)
    {
        return NULL;
    }
    esp_http_client_handle_t client = calloc(1, sizeof(*client));
    if (client == NULL)
    {
        return NULL;
    }
    client->url = strdup(config->url);
    client->handler = config->event_handler;
    client->user_data = config->user_data;
    client->timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 30000;
    client->disable_auto_redirect = config->disable_auto_redirect;
    atomic_store(&client->cancel, false);
    if (client->url == NULL)
    {
        free(client);
        return NULL;
    }
    return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value)
{
    if (client == NULL || key == NULL || value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *joined = malloc(strlen(key) + strlen(value) + 3U);
    if (joined == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    sprintf(joined, "%s: %s", key, value);
    char **grown = realloc(client->headers,
                           (client->header_count + 1U) * sizeof(*grown));
    if (grown == NULL)
    {
        free(joined);
        return ESP_ERR_NO_MEM;
    }
    client->headers = grown;
    client->headers[client->header_count++] = joined;
    return ESP_OK;
}

static size_t _header_cb(void *contents, size_t size, size_t nitems,
                         void *userp)
{
    esp_http_client_handle_t client = userp;
    size_t len = size * nitems;
    char *line = malloc(len + 1U);

    if (line == NULL)
    {
        return 0;
    }
    memcpy(line, contents, len);
    line[len] = '\0';
    while (len > 0U && (line[len - 1U] == '\r' || line[len - 1U] == '\n'))
    {
        line[--len] = '\0';
    }
    const char *colon = strchr(line, ':');
    esp_err_t result = ESP_OK;
    if (len > 0U && colon != NULL)
    {
        char *value = (char *)colon + 1;
        while (*value == ' ' || *value == '\t')
        {
            ++value;
        }
        line[colon - line] = '\0';
        result = _emit(client, HTTP_EVENT_ON_HEADER, NULL, 0, line, value);
    }
    free(line);
    return result == ESP_OK ? size * nitems : 0;
}

static size_t _write_cb(char *data, size_t size, size_t nitems, void *userp)
{
    esp_http_client_handle_t client = userp;
    const size_t len = size * nitems;

    if (atomic_load(&client->cancel))
    {
        return 0;
    }
    if (_emit(client, HTTP_EVENT_ON_DATA, data, (int)len, NULL, NULL) !=
            ESP_OK)
    {
        return 0;
    }
    return len;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    if (client == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    sim_http_script_t script;
    const bool have_script = _script_snapshot(client->url, &script);
    if (atomic_load(&s_script_only) && !have_script)
    {
        /* CI: the data plane is exclusively script-controlled; an
         * unregistered URL behaves like an offline radio. */
        return ESP_FAIL;
    }
    if (have_script)
    {
        esp_err_t script_result = ESP_OK;
        client->status_code = script.status;
        (void)_emit(client, HTTP_EVENT_ON_CONNECTED, NULL, 0, NULL, NULL);
        if (script.content_type != NULL)
        {
            (void)_emit(client, HTTP_EVENT_ON_HEADER, NULL, 0,
                        "Content-Type", script.content_type);
        }
        (void)_emit(client, HTTP_EVENT_ON_HEADERS_COMPLETE, NULL, 0, NULL,
                    NULL);
        if (script.size > 0U)
        {
            if (_emit(client, HTTP_EVENT_ON_DATA, script.body,
                      (int)script.size, NULL, NULL) != ESP_OK)
            {
                script_result = ESP_FAIL;
            }
        }
        if (script_result == ESP_OK)
        {
            (void)_emit(client, HTTP_EVENT_ON_FINISH, NULL, 0, NULL, NULL);
        }
        (void)_emit(client, HTTP_EVENT_DISCONNECTED, NULL, 0, NULL, NULL);
        _script_release(&script);
        return script_result;
    }
    CURL *curl = curl_easy_init();
    if (curl == NULL)
    {
        return ESP_FAIL;
    }
    struct curl_slist *hdrs = NULL;
    esp_err_t result = ESP_FAIL;

    for (size_t i = 0; i < client->header_count; i++)
    {
        hdrs = curl_slist_append(hdrs, client->headers[i]);
    }
    curl_easy_setopt(curl, CURLOPT_URL, client->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, client->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                     client->disable_auto_redirect ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, client);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, _header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, client);

    (void)_emit(client, HTTP_EVENT_ON_CONNECTED, NULL, 0, NULL, NULL);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    client->status_code = (int)status;

    if (code == CURLE_OK)
    {
        (void)_emit(client, HTTP_EVENT_ON_HEADERS_COMPLETE, NULL, 0, NULL,
                    NULL);
        (void)_emit(client, HTTP_EVENT_ON_FINISH, NULL, 0, NULL, NULL);
        result = ESP_OK;
    }
    else if (atomic_load(&client->cancel))
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        (void)_emit(client, HTTP_EVENT_ERROR, NULL, 0, NULL, NULL);
        result = ESP_FAIL;
    }
    (void)_emit(client, HTTP_EVENT_DISCONNECTED, NULL, 0, NULL, NULL);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return result;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return client == NULL ? 0 : client->status_code;
}

esp_err_t esp_http_client_cancel_request(esp_http_client_handle_t client)
{
    if (client == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&client->cancel, true);
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (client == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < client->header_count; i++)
    {
        free(client->headers[i]);
    }
    free(client->headers);
    free(client->url);
    free(client);
    return ESP_OK;
}

/** @file Agent TCP JSON-RPC server (127.0.0.1, one request per line).
 *
 * Response envelope: {"id":<echo>,"ok":true,"result":{...}} or
 * {"id":<echo>,"ok":false,"error":"<message>"}.
 */
#include <arpa/inet.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"

#include "app_manager.h"
#include "app_manager_builtin.h"
#include "app_manager_types.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "sim_agent.h"
#include "sim_agent_tree.h"
#include "sim_backends.h"
#include "sim_bsp.h"
#include "sim_http.h"
#include "sim_lv_adapter.h"
#include "sim_png.h"
#include "sim_time.h"
#include "weather_service.h"

#define SIM_AGENT_LINE_MAX (256 * 1024)

#include "sim_quit.h"

static _Atomic int s_listen_fd = -1;
static pthread_t s_accept_thread;
static atomic_bool s_running;
static pthread_mutex_t s_rpc_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_out_dir[256] = "shots";

static uint64_t _fb_hash(void)
{
    const uint16_t *fb = sim_bsp_framebuffer();
    const size_t words = (size_t)SIM_BSP_H_RES * SIM_BSP_V_RES;
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t i = 0; i < words; i++)
    {
        hash ^= fb[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool _safe_shot_name(const char *raw, char *out, size_t out_size)
{
    const char *name = ((raw != NULL) && (raw[0] != '\0')) ? raw : "agent.png";

    if ((strchr(name, '/') != NULL) || (strchr(name, '\\') != NULL) ||
            (strstr(name, "..") != NULL))
    {
        return false;
    }
    snprintf(out, out_size, "%s", name);
    return true;
}

static bool _wait_idle(uint32_t timeout_ms, uint64_t *hash_out,
                       uint32_t *steps_out)
{
    const uint32_t budget = (timeout_ms == 0U) ? 5000U : timeout_ms;
    uint32_t spent = 0U;
    uint32_t steps = 0U;
    uint64_t previous = 0U;
    bool have_previous = false;

    while (spent < budget)
    {
        uint64_t current;
        uint64_t after;

        if (sim_lv_ci_enabled())
        {
            if (sim_lv_ci_step(33U) != 0)
            {
                return false;
            }
            steps++;
            spent += 33U;
            current = _fb_hash();
            if (sim_lv_ci_step(33U) != 0)
            {
                return false;
            }
            steps++;
            spent += 33U;
            after = _fb_hash();
        }
        else
        {
            usleep(33000U);
            spent += 33U;
            current = _fb_hash();
            usleep(33000U);
            spent += 33U;
            after = _fb_hash();
        }
        if (lv_anim_count_running() == 0U && have_previous &&
                previous == current && current == after)
        {
            if (hash_out != NULL)
            {
                *hash_out = after;
            }
            if (steps_out != NULL)
            {
                *steps_out = steps;
            }
            return true;
        }
        previous = after;
        have_previous = true;
    }
    if (hash_out != NULL)
    {
        *hash_out = _fb_hash();
    }
    if (steps_out != NULL)
    {
        *steps_out = steps;
    }
    return false;
}

static cJSON *_handle(cJSON *request, bool *ok)
{
    const char *method = cJSON_GetStringValue(
                             cJSON_GetObjectItemCaseSensitive(request, "method"));
    cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    cJSON *result = cJSON_CreateObject();

    *ok = (method != NULL);
    if (result == NULL)
    {
        return NULL;
    }
    if (method == NULL)
    {
        *ok = false;
        cJSON_Delete(result);
        return NULL;
    }
    if (strcmp(method, "sim.ping") == 0)
    {
        cJSON_AddStringToObject(result, "version", "sim-m6");
        cJSON_AddNumberToObject(result, "width", SIM_BSP_H_RES);
        cJSON_AddNumberToObject(result, "height", SIM_BSP_V_RES);
        cJSON_AddBoolToObject(result, "ci", sim_lv_ci_enabled());
        cJSON_AddNumberToObject(result, "frames",
                                (double)sim_lv_frame_count());
        if (!sim_lv_ci_enabled())
        {
            /* The lifecycle query drains via the mailbox; only safe when the
             * worker free-runs (CI must read state from the tree instead). */
            cJSON_AddStringToObject(result, "active_app",
                                    app_manager_get_active_app_id());
        }
    }
    else if (strcmp(method, "sim.step") == 0)
    {
        const cJSON *ms = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                              params, "ms") : NULL;
        const uint32_t step_ms = (ms != NULL && cJSON_IsNumber(ms))
                                 ? (uint32_t)ms->valuedouble : 33U;
        if (!sim_lv_ci_enabled())
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "not in --ci mode");
        }
        else if ((step_ms % 33U) != 0U)
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "ms must be a multiple of 33");
        }
        else
        {
            *ok = (sim_lv_ci_step(step_ms) == 0);
            cJSON_AddNumberToObject(result, "frames",
                                    (double)sim_lv_frame_count());
        }
    }
    else if (strcmp(method, "sim.wait_idle") == 0)
    {
        const cJSON *to = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                              params, "timeout_ms") : NULL;
        uint64_t hash = 0U;
        uint32_t steps = 0U;
        const bool settled = _wait_idle((to != NULL && cJSON_IsNumber(to))
                                        ? (uint32_t)to->valuedouble : 5000U,
                                        &hash, &steps);
        cJSON_AddBoolToObject(result, "idle", settled);
        cJSON_AddNumberToObject(result, "hash", (double)hash);
        cJSON_AddNumberToObject(result, "steps", steps);
        *ok = true;
    }
    else if (strcmp(method, "sim.screenshot") == 0)
    {
        const cJSON *name = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                params, "name") : NULL;
        const cJSON *wi = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                              params, "wait_idle") : NULL;
        char path[512];
        char *pixels = NULL;

        if ((wi == NULL) || cJSON_IsTrue(wi))
        {
            (void)_wait_idle(5000U, NULL, NULL);
        }
        (void)mkdir(s_out_dir, 0775);
        {
            char shot_name[128];
            const char *raw = (name != NULL && cJSON_IsString(name))
                              ? name->valuestring : NULL;
            if (!_safe_shot_name(raw, shot_name, sizeof(shot_name)))
            {
                *ok = false;
                cJSON_AddStringToObject(result, "error", "invalid name");
                return result;
            }
            snprintf(path, sizeof(path), "%s/%s", s_out_dir, shot_name);
        }
        pixels = malloc((size_t)SIM_BSP_H_RES * SIM_BSP_V_RES * 2U);
        if (pixels == NULL)
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "no memory");
        }
        else
        {
            (void)esp_lv_adapter_lock(-1);
            memcpy(pixels, sim_bsp_framebuffer(),
                   (size_t)SIM_BSP_H_RES * SIM_BSP_V_RES * 2U);
            (void)esp_lv_adapter_unlock();
            *ok = (sim_png_save_rgb565(path, (const uint16_t *)pixels,
                                       SIM_BSP_H_RES, SIM_BSP_V_RES, 1) == 0);
            free(pixels);
            if (*ok)
            {
                cJSON_AddStringToObject(result, "path", path);
            }
        }
    }
    else if (strcmp(method, "sim.tree") == 0)
    {
        char *tree = NULL;

        (void)esp_lv_adapter_lock(-1);
        tree = sim_agent_tree_dump_active_screen();
        (void)esp_lv_adapter_unlock();
        if (tree == NULL)
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "tree dump failed");
        }
        else
        {
            cJSON *parsed = cJSON_Parse(tree);
            free(tree);
            if (parsed == NULL)
            {
                *ok = false;
            }
            else
            {
                cJSON_AddItemToObject(result, "tree", parsed);
            }
        }
    }
    else if (strcmp(method, "sim.touch") == 0)
    {
        const cJSON *action = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                  params, "action") : NULL;
        const cJSON *x = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                             params, "x") : NULL;
        const cJSON *y = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                             params, "y") : NULL;
        const char *act = cJSON_GetStringValue(action);
        if ((act == NULL) || !cJSON_IsNumber(x) || !cJSON_IsNumber(y))
        {
            *ok = false;
        }
        else
        {
            const bool pressed = (strcmp(act, "down") == 0) ||
                                 (strcmp(act, "move") == 0);
            sim_lv_touch_update((int16_t)(int)x->valuedouble,
                                (int16_t)(int)y->valuedouble, pressed);
        }
    }
    else if (strcmp(method, "sim.key") == 0)
    {
        const cJSON *button = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                  params, "button") : NULL;
        const cJSON *action = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                  params, "action") : NULL;
        const char *btn = cJSON_GetStringValue(button);
        const char *act = cJSON_GetStringValue(action);
        const sim_key_t key = ((btn != NULL) && strcmp(btn, "power") == 0)
                              ? SIM_KEY_POWER : SIM_KEY_BOOT;
        *ok = (btn != NULL && act != NULL);
        if (*ok)
        {
            if (strcmp(act, "click") == 0)
            {
                (void)sim_bsp_key(key, SIM_KEY_ACTION_PRESS);
                (void)sim_bsp_key(key, SIM_KEY_ACTION_RELEASE);
            }
            else
            {
                (void)sim_bsp_key(key, (strcmp(act, "release") == 0)
                                  ? SIM_KEY_ACTION_RELEASE : SIM_KEY_ACTION_PRESS);
            }
        }
    }
    else if (strcmp(method, "sim.navigate") == 0)
    {
        const cJSON *app = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                               params, "app") : NULL;
        const char *app_id = cJSON_GetStringValue(app);
        if (app_id == NULL)
        {
            *ok = false;
        }
        else
        {
            app_manager_nav_request_t nav =
            {
                .operation = APP_MANAGER_NAV_OP_RUN,
                .app_id = app_id,
                .transition = { .effect = APP_MANAGER_TRANSITION_NONE },
            };
            const cJSON *page = cJSON_GetObjectItemCaseSensitive(
                                    params, "page");
            if (cJSON_IsString(page))
            {
                nav.page_id = page->valuestring;
                nav.operation = APP_MANAGER_NAV_OP_OPEN_PAGE;
            }
            const esp_err_t res = app_manager_navigate_async(&nav, NULL, NULL);
            *ok = (res == ESP_OK);
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error",
                                        esp_err_to_name(res));
            }
        }
    }
    else if (strcmp(method, "sim.set_time") == 0)
    {
        const cJSON *epoch = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                 params, "epoch") : NULL;
        if (epoch == NULL)
        {
            *ok = false;
        }
        else
        {
            (void)sim_time_set_epoch((int64_t)epoch->valuedouble);
        }
    }
    else if (strcmp(method, "sim.set_power") == 0)
    {
        const cJSON *v = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                             params, "voltage") : NULL;
        const cJSON *p = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                             params, "pct") : NULL;
        const cJSON *c = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                             params, "charging") : NULL;
        if ((v == NULL) || (p == NULL))
        {
            *ok = false;
        }
        else
        {
            sim_backends_set_power((uint16_t)(int)v->valuedouble,
                                   (int8_t)(int)p->valuedouble,
                                   cJSON_IsTrue(c),
                                   cJSON_IsTrue(
                                       cJSON_GetObjectItemCaseSensitive(
                                           params, "vbus")));
        }
    }
    else if (strcmp(method, "sim.set_imu") == 0)
    {
        const cJSON *pitch = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                 params, "pitch") : NULL;
        const cJSON *roll = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                params, "roll") : NULL;
        if ((pitch == NULL) || (roll == NULL))
        {
            *ok = false;
        }
        else
        {
            sim_backends_set_imu((int)(pitch->valuedouble * 100.0),
                                 (int)(roll->valuedouble * 100.0));
        }
    }
    else if (strcmp(method, "sim.set_wifi") == 0)
    {
        const cJSON *state = params != NULL ?
                             cJSON_GetObjectItemCaseSensitive(params, "state") : NULL;
        const char *value = cJSON_GetStringValue(state);
        if (value == NULL)
        {
            *ok = false;
        }
        else
        {
            const bool connected = (strcmp(value, "connected") == 0);
            (void)weather_service_set_network_ready(connected,
                                                    connected ? 0x0A000001U : 0U);
        }
    }
    else if (strcmp(method, "sim.set_weather") == 0)
    {
        const cJSON *endpoint = params != NULL ?
                                cJSON_GetObjectItemCaseSensitive(params, "endpoint") : NULL;
        const cJSON *status = params != NULL ?
                              cJSON_GetObjectItemCaseSensitive(params, "status") : NULL;
        const cJSON *body = params != NULL ?
                            cJSON_GetObjectItemCaseSensitive(params, "body") : NULL;
        const char *path = cJSON_GetStringValue(endpoint);
        const char *json = cJSON_GetStringValue(body);
        if ((path == NULL) || (json == NULL))
        {
            *ok = false;
        }
        else
        {
            char match[64];
            if (strcmp(path, "location") == 0)
            {
                snprintf(match, sizeof(match), "/api/v1/location");
            }
            else
            {
                snprintf(match, sizeof(match), "/api/v1/weather/%s", path);
            }
            *ok = (sim_http_set_response(match,
                                         (status != NULL) ? status->valueint : 200,
                                         "application/json",
                                         (const uint8_t *)json,
                                         strlen(json)) == 0);
            if (*ok)
            {
                (void)weather_service_request_refresh();
            }
        }
    }
    else if (strcmp(method, "sim.pause") == 0)
    {
        const cJSON *enabled = params != NULL ?
                               cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
        *ok = true;
        if (enabled != NULL && cJSON_IsTrue(enabled))
        {
            (void)esp_lv_adapter_pause(-1);
        }
        else
        {
            (void)esp_lv_adapter_resume();
        }
    }
    else if (strcmp(method, "sim.pm") == 0)
    {
        const cJSON *off = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                               params, "off_ms") : NULL;
        const cJSON *standby = params != NULL ?
                               cJSON_GetObjectItemCaseSensitive(params, "standby_ms") : NULL;
        const cJSON *want_get = params != NULL ?
                                cJSON_GetObjectItemCaseSensitive(params, "get") : NULL;
        if (cJSON_IsTrue(want_get))
        {
            *ok = true;
            cJSON_AddNumberToObject(result, "pm_state",
                                    (double)app_manager_pm_get_state());
            cJSON_AddNumberToObject(result, "off_ms",
                                    (double)app_manager_pm_get_timeout_ms());
            cJSON_AddNumberToObject(result, "standby_ms",
                                    (double)app_manager_pm_get_standby_delay_ms());
        }
        *ok = (cJSON_IsNumber(off) || cJSON_IsNumber(standby) ||
               cJSON_IsTrue(want_get));
        if (*ok && cJSON_IsNumber(off))
        {
            const esp_err_t res = app_manager_pm_set_timeout_ms(
                                      (int32_t)off->valuedouble);
            *ok = (res == ESP_OK);
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error",
                                        esp_err_to_name(res));
            }
        }
        if (*ok && cJSON_IsNumber(standby))
        {
            const esp_err_t res = app_manager_pm_set_standby_delay_ms(
                                      (int32_t)standby->valuedouble);
            *ok = (res == ESP_OK);
        }
    }
    else if (strcmp(method, "sim.exit") == 0)
    {
        atomic_store(&sim_quit_flag, 1);
    }
    else if (strcmp(method, "sim.apps") == 0)
    {
        extern const app_manager_app_desc_t _app_manager_apps_start[];
        extern const app_manager_app_desc_t _app_manager_apps_end[];
        cJSON *apps = cJSON_AddArrayToObject(result, "apps");

        for (const app_manager_app_desc_t *d = _app_manager_apps_start;
                d < _app_manager_apps_end && apps != NULL; d++)
        {
            cJSON *item = cJSON_CreateObject();
            if (item == NULL)
            {
                break;
            }
            cJSON_AddStringToObject(item, "app_id",
                                    d->id != NULL ? d->id : "");
            cJSON_AddStringToObject(item, "name",
                                    d->name != NULL ? d->name : "");
            cJSON_AddStringToObject(item, "root_page",
                                    d->root_page_id != NULL ?
                                    d->root_page_id : "");
            cJSON_AddItemToArray(apps, item);
        }
        if (!sim_lv_ci_enabled())
        {
            cJSON_AddStringToObject(result, "active",
                                    app_manager_get_active_app_id());
        }
    }
    else
    {
        *ok = false;
        cJSON_AddStringToObject(result, "error", "unknown method");
    }
    return result;
}

static void *_client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    FILE *stream = fdopen(fd, "r+");
    char *line = malloc(SIM_AGENT_LINE_MAX);

    if ((stream == NULL) || (line == NULL))
    {
        if (stream != NULL)
        {
            fclose(stream);
        }
        else
        {
            (void)close(fd);
        }
        free(line);
        return NULL;
    }
    while (atomic_load(&s_running) &&
            fgets(line, SIM_AGENT_LINE_MAX, stream) != NULL)
    {
        cJSON *request = cJSON_Parse(line);
        cJSON *reply = cJSON_CreateObject();
        bool ok = false;
        cJSON *result = NULL;
        char *text = NULL;

        if (reply == NULL)
        {
            cJSON_Delete(request);
            continue;
        }
        if (request == NULL)
        {
            const char *err = "{\"ok\":false,\"error\":\"invalid json\"}";
            cJSON_Delete(reply);
            (void)!write(fd, err, strlen(err));
            (void)!write(fd, "\n", 1);
            continue;
        }
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(request, "id");
        const double id_value = cJSON_IsNumber(rid) ? rid->valuedouble : 0.0;
        cJSON_AddNumberToObject(reply, "id", id_value);
        (void)pthread_mutex_lock(&s_rpc_lock);
        result = _handle(request, &ok);
        (void)pthread_mutex_unlock(&s_rpc_lock);
        if (!cJSON_IsNumber(rid))
        {
            cJSON_DeleteItemFromObject(reply, "id");
        }
        cJSON_AddBoolToObject(reply, "ok", ok);
        if (result != NULL)
        {
            cJSON_AddItemToObject(reply, "result", result);
        }
        text = cJSON_PrintUnformatted(reply);
        if (text != NULL)
        {
            (void)!write(fd, text, strlen(text));
            (void)!write(fd, "\n", 1);
            free(text);
        }
        cJSON_Delete(request);
        cJSON_Delete(reply);
    }
    fclose(stream);
    free(line);
    return NULL;
}

static void *_accept_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&s_running))
    {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int listen_fd = atomic_load(&s_listen_fd);
        const int fd = accept(listen_fd, (struct sockaddr *)&peer,
                              &peer_len);
        pthread_t worker;

        if (fd < 0)
        {
            if (listen_fd < 0 || !atomic_load(&s_running))
            {
                break;
            }
            usleep(20000U);
            continue;
        }
        if (pthread_create(&worker, NULL, _client_thread,
                           (void *)(intptr_t)fd) == 0)
        {
            (void)pthread_detach(worker);
        }
        else
        {
            (void)close(fd);
        }
    }
    return NULL;
}

bool sim_agent_start(int port, const char *out_dir)
{
    struct sockaddr_in addr;

    if (out_dir != NULL)
    {
        snprintf(s_out_dir, sizeof(s_out_dir), "%s", out_dir);
    }
    const int created = socket(AF_INET, SOCK_STREAM, 0);
    atomic_store(&s_listen_fd, created);
    if (created < 0)
    {
        return false;
    }
    {
        int reuse = 1;
        (void)setsockopt(created, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof(reuse));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(created, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        (void)close(created);
        atomic_store(&s_listen_fd, -1);
        return false;
    }
    if (listen(created, 16) != 0)
    {
        (void)close(created);
        atomic_store(&s_listen_fd, -1);
        return false;
    }
    atomic_store(&s_running, true);
    if (pthread_create(&s_accept_thread, NULL, _accept_thread, NULL) != 0)
    {
        atomic_store(&s_running, false);
        (void)close(created);
        atomic_store(&s_listen_fd, -1);
        return false;
    }
    printf("agent: listening on 127.0.0.1:%d\n", port);
    return true;
}

void sim_agent_stop(void)
{
    if (atomic_load(&s_running))
    {
        atomic_store(&s_running, false);
        const int fd = atomic_exchange(&s_listen_fd, -1);
        if (fd >= 0)
        {
            (void)shutdown(fd, SHUT_RDWR);
            (void)close(fd);
        }
        (void)pthread_join(s_accept_thread, NULL);
    }
}

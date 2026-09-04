/** @file Agent TCP JSON-RPC server (127.0.0.1, one request per line).
 *
 * Response envelope: {"id":<echo>,"ok":true,"result":{...}} or
 * {"id":<echo>,"ok":false,"error":"<message>"}.
 */
#include <arpa/inet.h>
#include <limits.h>
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
#include "app_manager_navigation.h"
#include "app_manager_task_switcher.h"
#include "app_manager_types.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "sim_agent.h"
#include "sim_agent_tree.h"
#include "sim_backends.h"
#include "sim_sd_host.h"
#include "recorder_service.h"
#include "nv_storage.h"
#include "connectivity_manager.h"
#include "wifi_service.h"
#include "sim_bsp.h"
#include "sim_http.h"
#include "sim_lv_adapter.h"
#include "sim_png.h"
#include "sim_time.h"
#include "weather_service.h"
#include "host_device_link_service.h"
#include "host_wifi_port.h"
#include "connectivity_manager.h"

#define SIM_AGENT_LINE_MAX (256 * 1024)

#include "sim_quit.h"

static _Atomic int s_listen_fd = -1;
static pthread_t s_accept_thread;
static atomic_bool s_running;
static pthread_mutex_t s_rpc_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_out_dir[256] = "shots";

typedef enum sim_wait_idle_result
{
    SIM_WAIT_IDLE_SETTLED = 0,
    SIM_WAIT_IDLE_TIMEOUT,
    SIM_WAIT_IDLE_PAUSED,
    SIM_WAIT_IDLE_FAILED,
} sim_wait_idle_result_t;

static sim_wait_idle_result_t _fb_state(uint64_t *hash_out,
                                        uint32_t *animations_out)
{
    const uint16_t *fb = sim_bsp_framebuffer();
    const size_t words = (size_t)SIM_BSP_H_RES * SIM_BSP_V_RES;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (esp_lv_adapter_lock(-1) != ESP_OK)
    {
        return SIM_WAIT_IDLE_FAILED;
    }
    if (sim_lv_paused())
    {
        esp_lv_adapter_unlock();
        return SIM_WAIT_IDLE_PAUSED;
    }
    for (size_t i = 0; i < words; i++)
    {
        hash ^= fb[i];
        hash *= UINT64_C(1099511628211);
    }
    if (hash_out != NULL)
    {
        *hash_out = hash;
    }
    if (animations_out != NULL)
    {
        *animations_out = lv_anim_count_running();
    }
    esp_lv_adapter_unlock();
    return SIM_WAIT_IDLE_SETTLED;
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

static bool _number_in_range(const cJSON *item, double minimum,
                             double maximum)
{
    return cJSON_IsNumber(item) && (item->valuedouble >= minimum) &&
           (item->valuedouble <= maximum);
}

static bool _optional_bool(const cJSON *item)
{
    return (item == NULL) || cJSON_IsBool(item);
}

static esp_err_t _restart_recorder_service(bool want_mounted)
{
    const recorder_service_config_t config =
    {
        .directory = host_sd_recordings_dir(),
        .max_duration_seconds = 30U * 60U,
        .minimum_free_bytes = 8U * 1024U * 1024U,
    };

    (void)recorder_service_deinit();
    if (!want_mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return recorder_service_init(&config);
}

static const char *_wait_idle_error(sim_wait_idle_result_t result)
{
    if (result == SIM_WAIT_IDLE_PAUSED)
    {
        return "adapter paused";
    }
    if (result == SIM_WAIT_IDLE_FAILED)
    {
        return "wait idle failed";
    }
    return "wait idle timed out";
}

static void _add_weather_status(cJSON *result)
{
    weather_service_status_snapshot_t status = {0};

    if (weather_service_get_status(&status) != ESP_OK)
    {
        cJSON_AddBoolToObject(result, "network_ready", false);
        cJSON_AddNumberToObject(result, "weather_state",
                                WEATHER_SERVICE_STATE_ERROR);
        cJSON_AddNumberToObject(result, "weather_failure",
                                WEATHER_SERVICE_FAILURE_INTERNAL);
        return;
    }
    cJSON_AddBoolToObject(result, "network_ready", status.network_ready);
    cJSON_AddNumberToObject(result, "weather_state", status.state);
    cJSON_AddNumberToObject(result, "weather_failure", status.failure);
}

static sim_wait_idle_result_t _wait_idle(uint32_t timeout_ms,
        uint64_t *hash_out, uint32_t *steps_out)
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
        uint32_t animations;
        sim_wait_idle_result_t state;

        if (sim_lv_paused())
        {
            return SIM_WAIT_IDLE_PAUSED;
        }

        if (sim_lv_ci_enabled())
        {
            if (sim_lv_ci_step(33U) != 0)
            {
                return SIM_WAIT_IDLE_FAILED;
            }
            steps++;
            spent += 33U;
            state = _fb_state(&current, NULL);
            if (state != SIM_WAIT_IDLE_SETTLED)
            {
                return state;
            }
            if (sim_lv_ci_step(33U) != 0)
            {
                return SIM_WAIT_IDLE_FAILED;
            }
            steps++;
            spent += 33U;
            state = _fb_state(&after, &animations);
        }
        else
        {
            usleep(33000U);
            spent += 33U;
            state = _fb_state(&current, NULL);
            if (state != SIM_WAIT_IDLE_SETTLED)
            {
                return state;
            }
            usleep(33000U);
            spent += 33U;
            state = _fb_state(&after, &animations);
        }
        if (state != SIM_WAIT_IDLE_SETTLED)
        {
            return state;
        }
        if (animations == 0U && have_previous &&
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
            return SIM_WAIT_IDLE_SETTLED;
        }
        previous = after;
        have_previous = true;
    }
    if (hash_out != NULL)
    {
        const sim_wait_idle_result_t state = _fb_state(hash_out, NULL);
        if (state != SIM_WAIT_IDLE_SETTLED)
        {
            return state;
        }
    }
    if (steps_out != NULL)
    {
        *steps_out = steps;
    }
    return SIM_WAIT_IDLE_TIMEOUT;
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
        _add_weather_status(result);
        if (!sim_lv_ci_enabled())
        {
            /* The lifecycle query drains via the mailbox; only safe when the
             * worker free-runs (CI must read state from the tree instead). */
            const char *active_app = app_manager_get_active_app_id();
            if (active_app != NULL)
            {
                cJSON_AddStringToObject(result, "active_app", active_app);
            }
            else
            {
                cJSON_AddNullToObject(result, "active_app");
            }
        }
        else
        {
            cJSON_AddNullToObject(result, "active_app");
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
        sim_wait_idle_result_t wait_result;

        if ((to != NULL) && !_number_in_range(to, 0.0, UINT32_MAX))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid timeout_ms");
        }
        else
        {
            wait_result = _wait_idle((to != NULL) ? (uint32_t)to->valuedouble
                                     : 5000U, &hash, &steps);
            if ((wait_result == SIM_WAIT_IDLE_PAUSED) ||
                    (wait_result == SIM_WAIT_IDLE_FAILED))
            {
                *ok = false;
                cJSON_AddStringToObject(result, "error",
                                        _wait_idle_error(wait_result));
            }
            else
            {
                cJSON_AddBoolToObject(result, "idle",
                                      wait_result == SIM_WAIT_IDLE_SETTLED);
                cJSON_AddNumberToObject(result, "hash", (double)hash);
                cJSON_AddNumberToObject(result, "steps", steps);
                *ok = true;
            }
        }
    }
    else if (strcmp(method, "sim.screenshot") == 0)
    {
        const cJSON *name = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                params, "name") : NULL;
        const cJSON *wi = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                              params, "wait_idle") : NULL;
        char path[512];
        char *pixels = NULL;

        if (((name != NULL) && !cJSON_IsString(name)) || !_optional_bool(wi))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid screenshot params");
            return result;
        }
        if ((wi == NULL) || cJSON_IsTrue(wi))
        {
            const sim_wait_idle_result_t wait_result = _wait_idle(5000U, NULL,
                NULL);
            if (wait_result != SIM_WAIT_IDLE_SETTLED)
            {
                *ok = false;
                cJSON_AddStringToObject(result, "error",
                                        _wait_idle_error(wait_result));
                return result;
            }
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
            const esp_err_t lock_result = esp_lv_adapter_lock(-1);
            if (lock_result == ESP_OK)
            {
                memcpy(pixels, sim_bsp_framebuffer(),
                       (size_t)SIM_BSP_H_RES * SIM_BSP_V_RES * 2U);
                esp_lv_adapter_unlock();
                *ok = (sim_png_save_rgb565(path, (const uint16_t *)pixels,
                                           SIM_BSP_H_RES, SIM_BSP_V_RES, 1) == 0);
            }
            else
            {
                *ok = false;
            }
            free(pixels);
            if (*ok)
            {
                cJSON_AddStringToObject(result, "path", path);
            }
            else
            {
                cJSON_AddStringToObject(result, "error", "screenshot failed");
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
        const bool valid_action = (act != NULL) &&
                                  ((strcmp(act, "down") == 0) ||
                                   (strcmp(act, "move") == 0) ||
                                   (strcmp(act, "up") == 0));
        if (!valid_action || !_number_in_range(x, 0.0, SIM_BSP_H_RES - 1U) ||
                !_number_in_range(y, 0.0, SIM_BSP_V_RES - 1U))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid touch params");
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
        const bool valid_button = (btn != NULL) &&
                                  ((strcmp(btn, "boot") == 0) ||
                                   (strcmp(btn, "power") == 0));
        const bool valid_action = (act != NULL) &&
                                  ((strcmp(act, "press") == 0) ||
                                   (strcmp(act, "release") == 0) ||
                                   (strcmp(act, "click") == 0));
        const sim_key_t key = ((btn != NULL) && (strcmp(btn, "power") == 0))
                              ? SIM_KEY_POWER : SIM_KEY_BOOT;
        *ok = valid_button && valid_action;
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
        else
        {
            cJSON_AddStringToObject(result, "error", "invalid key params");
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
    else if (strcmp(method, "sim.switcher") == 0)
    {
        /* The switcher is normally opened by a physical HOME double-press;
         * the CI harness cannot reproduce that timing reliably, so expose the
         * same gateway system command for deterministic review. */
        const cJSON *get = params != NULL ?
                           cJSON_GetObjectItemCaseSensitive(params, "get") :
                           NULL;
        if (cJSON_IsTrue(get))
        {
            cJSON_AddBoolToObject(result, "visible",
                                  app_manager_task_switcher_is_visible());
            *ok = true;
        }
        else
        {
            const cJSON *action = params != NULL ?
                                  cJSON_GetObjectItemCaseSensitive(
                                      params, "action") :
                                  NULL;
            const char *act = cJSON_GetStringValue(action);
            const bool clear_all = (act != NULL) &&
                                   (strcmp(act, "clear_all") == 0);
            const esp_err_t res = app_manager_navigation_submit_system_op(
                                      clear_all ?
                                      APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_CLEAR_ALL :
                                      APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW,
                                      NULL, NULL);
            *ok = (res == ESP_OK);
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(res));
            }
        }
    }
    else if (strcmp(method, "sim.set_time") == 0)
    {
        const cJSON *epoch = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                 params, "epoch") : NULL;
        if (!_number_in_range(epoch, 0.0, (double)INT64_MAX - 1024.0))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid epoch");
        }
        else
        {
            const esp_err_t res = sim_time_set_epoch((int64_t)epoch->valuedouble);
            *ok = (res == ESP_OK);
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(res));
            }
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
        const cJSON *vbus = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                params, "vbus") : NULL;
        if ((!cJSON_IsNumber(v) && !cJSON_IsNumber(p)) ||
                !((v == NULL) || _number_in_range(v, 0.0, UINT16_MAX)) ||
                !((p == NULL) || _number_in_range(p, 0.0, 100.0)) ||
                !_optional_bool(c) || !_optional_bool(vbus))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid power params");
        }
        else
        {
            sim_backends_set_power(cJSON_IsNumber(v) ?
                                   (uint16_t)(int)v->valuedouble : 0U,
                                   cJSON_IsNumber(p) ?
                                   (int8_t)(int)p->valuedouble : (int8_t) -1,
                                   cJSON_IsTrue(c),
                                   cJSON_IsTrue(vbus));
        }
    }
    else if (strcmp(method, "sim.set_imu") == 0)
    {
        const cJSON *pitch = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                 params, "pitch") : NULL;
        const cJSON *roll = params != NULL ? cJSON_GetObjectItemCaseSensitive(
                                params, "roll") : NULL;
        if (!_number_in_range(pitch, (double)INT_MIN / 100.0,
                              (double)INT_MAX / 100.0) ||
                !_number_in_range(roll, (double)INT_MIN / 100.0,
                                  (double)INT_MAX / 100.0))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid imu params");
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
        const bool valid_state = (value != NULL) &&
                                 ((strcmp(value, "connected") == 0) ||
                                  (strcmp(value, "disconnected") == 0));
        if (!valid_state)
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid wifi state");
        }
        else
        {
            const bool connected = (strcmp(value, "connected") == 0);
            const esp_err_t res = weather_service_set_network_ready(
                                      connected, connected ? 0x0A000001U : 0U);
            *ok = (res == ESP_OK);
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(res));
            }
            else
            {
                _add_weather_status(result);
            }
        }
    }
    else if (strcmp(method, "sim.set_bluetooth") == 0)
    {
        device_link_service_status_t status;
        if (device_link_service_get_status(&status) != ESP_OK)
        {
            memset(&status, 0, sizeof(status));
            status.generation = 0U;
        }
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(params,
                               "enabled");
        const cJSON *bound = cJSON_GetObjectItemCaseSensitive(params, "bound");
        const cJSON *active = cJSON_GetObjectItemCaseSensitive(params, "active");
        const cJSON *connected = cJSON_GetObjectItemCaseSensitive(
                                     params, "client_connected");
        const cJSON *remaining = cJSON_GetObjectItemCaseSensitive(
                                     params, "window_remaining_ms");
        status.available = true;
        if (cJSON_IsBool(enabled))
        {
            status.enabled = cJSON_IsTrue(enabled);
        }
        if (cJSON_IsBool(bound))
        {
            status.bound = cJSON_IsTrue(bound);
        }
        if (cJSON_IsBool(active))
        {
            status.active = cJSON_IsTrue(active);
        }
        if (cJSON_IsBool(connected))
        {
            status.client_connected = cJSON_IsTrue(connected);
        }
        if (cJSON_IsNumber(remaining))
        {
            status.window_remaining_ms = (uint32_t)remaining->valuedouble;
        }
        status.state = status.active ? DEVICE_LINK_SERVICE_STATE_WINDOW :
                       (status.enabled ? DEVICE_LINK_SERVICE_STATE_ADVERTISING :
                        DEVICE_LINK_SERVICE_STATE_DISABLED);
        status.pending_confirmation = false;
        status.confirmation_token = 0U;
        status.generation = status.generation + 1U;
        *ok = (host_device_link_service_publish_status(&status) == ESP_OK);
    }
    else if (strcmp(method, "sim.offer_pairing") == 0)
    {
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(params, "token");
        const cJSON *passkey = cJSON_GetObjectItemCaseSensitive(params,
                               "passkey");
        const device_link_confirmation_token_t use_token =
            cJSON_IsNumber(token) ? (device_link_confirmation_token_t)
            token->valuedouble : 0x1234U;
        const uint32_t use_passkey = cJSON_IsNumber(passkey) ?
                                     (uint32_t)passkey->valuedouble : 482913U;
        *ok = (host_device_link_service_offer_numeric_comparison(use_token,
               use_passkey) == ESP_OK);
    }
    else if (strcmp(method, "sim.set_wifi_scan") == 0)
    {
        const cJSON *records = cJSON_GetObjectItemCaseSensitive(params,
                               "records");
        wifi_service_port_scan_record_t port_records[WIFI_SERVICE_MAX_SCAN_RECORDS];
        size_t count = 0U;
        bool valid = cJSON_IsArray(records);
        if (valid)
        {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, records)
            {
                if (count >= WIFI_SERVICE_MAX_SCAN_RECORDS)
                {
                    valid = false;
                    break;
                }
                const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(item,
                                    "ssid");
                const char *text = cJSON_GetStringValue(ssid);
                if (text == NULL)
                {
                    valid = false;
                    break;
                }
                memset(&port_records[count], 0, sizeof(port_records[count]));
                size_t length = strlen(text);
                if (length > WIFI_SERVICE_SSID_MAX_BYTES)
                {
                    length = WIFI_SERVICE_SSID_MAX_BYTES;
                }
                memcpy(port_records[count].ssid, text, length);
                port_records[count].ssid_length = (uint8_t)length;
                const cJSON *rssi = cJSON_GetObjectItemCaseSensitive(item,
                                    "rssi");
                port_records[count].rssi = cJSON_IsNumber(rssi) ?
                                           (int8_t)rssi->valuedouble : (int8_t) -55;
                const cJSON *channel = cJSON_GetObjectItemCaseSensitive(item,
                                       "channel");
                port_records[count].channel = cJSON_IsNumber(channel) ?
                                              (uint8_t)channel->valuedouble : 6U;
                const cJSON *security = cJSON_GetObjectItemCaseSensitive(item,
                                        "security");
                const char *sec = cJSON_GetStringValue(security);
                port_records[count].security =
                    (sec != NULL && strcmp(sec, "open") == 0) ?
                    WIFI_SERVICE_SECURITY_OPEN : WIFI_SERVICE_SECURITY_PERSONAL;
                ++count;
            }
        }
        if (valid && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,
                                  "request")))
        {
            connectivity_manager_operation_id_t op = 0U;
            const esp_err_t request_result =
                connectivity_manager_request_scan(&op);
            if (request_result != ESP_OK)
            {
                *ok = false;
                cJSON_AddStringToObject(result, "error",
                                        esp_err_to_name(request_result));
                valid = false;
            }
        }
        if (valid)
        {
            host_wifi_port_set_scan_records(port_records, count,
                                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                                    params, "truncated")));
            const cJSON *trigger = cJSON_GetObjectItemCaseSensitive(params,
                                   "trigger");
            esp_err_t scan_result = ESP_OK;
            if (cJSON_IsTrue(trigger))
            {
                const cJSON *status = cJSON_GetObjectItemCaseSensitive(params,
                                      "status");
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,
                                 "wait_scan")))
                {
                    /* The connectivity worker picks the request up on its own
                     * cadence; keep delivering SCAN_DONE while a port scan
                     * owns the radio so the completion cannot race ahead of
                     * or miss the window. */
                    bool owned_seen = false;
                    for (int attempt = 0; attempt < 600; ++attempt)
                    {
                        if (host_wifi_port_scan_owned())
                        {
                            owned_seen = true;
                            (void)host_wifi_port_complete_scan(
                                cJSON_IsNumber(status) ?
                                (int32_t)status->valuedouble : 0);
                        }
                        else if (owned_seen)
                        {
                            break;
                        }
                        usleep(50000U);
                    }
                }
                else
                {
                    scan_result = host_wifi_port_complete_scan(
                                      cJSON_IsNumber(status) ?
                                      (int32_t)status->valuedouble : 0);
                }
            }
            cJSON_AddNumberToObject(result, "scan_owned",
                                    (double)host_wifi_port_scan_owned());
            cJSON_AddNumberToObject(result, "scan_id",
                                    (double)host_wifi_port_scan_id());
            cJSON_AddNumberToObject(result, "epoch",
                                    (double)host_wifi_port_epoch());
            if (scan_result != ESP_OK)
            {
                *ok = false;
                cJSON_AddStringToObject(result, "error",
                                        esp_err_to_name(scan_result));
            }
            else if (cJSON_IsTrue(trigger))
            {
                /* Confirm the manager actually consumed the completion and
                 * cached the records before replying, so callers never race
                 * the publish cadence. */
                connectivity_manager_scan_snapshot_t done;
                bool settled = false;
                for (int attempt = 0; attempt < 200 && !settled; ++attempt)
                {
                    usleep(50000U);
                    if (connectivity_manager_get_scan_snapshot(&done) ==
                            ESP_OK && !done.running)
                    {
                        settled = true;
                    }
                }
                cJSON_AddNumberToObject(result, "settled_records",
                                        settled ?
                                        (double)done.record_count : -1.0);
            }
            else
            {
                *ok = true;
            }
        }
        else
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid scan records");
        }
    }
    else if (strcmp(method, "sim.connectivity") == 0)
    {
        connectivity_manager_status_snapshot_t status;
        connectivity_manager_scan_snapshot_t scan;
        memset(&status, 0, sizeof(status));
        memset(&scan, 0, sizeof(scan));
        const esp_err_t status_result = connectivity_manager_get_status(
                                            &status);
        const esp_err_t scan_result = connectivity_manager_get_scan_snapshot(
                                          &scan);
        if (status_result != ESP_OK || scan_result != ESP_OK)
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "no snapshot");
        }
        else
        {
            cJSON_AddNumberToObject(result, "state", (double)status.state);
            cJSON_AddNumberToObject(result, "failure",
                                    (double)status.failure);
            cJSON_AddNumberToObject(result, "last_error",
                                    (double)status.last_error);
            cJSON_AddBoolToObject(result, "available", status.available);
            cJSON_AddBoolToObject(result, "radio_available",
                                  status.radio_available);
            cJSON_AddBoolToObject(result, "saved_profile",
                                  status.saved_profile);
            cJSON_AddBoolToObject(result, "auto_connect",
                                  status.auto_connect);
            cJSON_AddBoolToObject(result, "manual_hold", status.manual_hold);
            cJSON_AddNumberToObject(result, "scan_state_running",
                                    (double)scan.running);
            cJSON_AddNumberToObject(result, "scan_records",
                                    (double)scan.record_count);
            cJSON_AddNumberToObject(result, "scan_error",
                                    (double)scan.last_error);
            wifi_service_status_snapshot_t wifi_status;
            wifi_service_scan_snapshot_t wifi_scan;
            memset(&wifi_status, 0, sizeof(wifi_status));
            memset(&wifi_scan, 0, sizeof(wifi_scan));
            cJSON_AddBoolToObject(result, "wifi_available",
                                  wifi_service_is_available());
            if (wifi_service_get_status(&wifi_status) == ESP_OK)
            {
                cJSON_AddNumberToObject(result, "wifi_state",
                                        (double)wifi_status.state);
                cJSON_AddNumberToObject(result, "wifi_error",
                                        (double)wifi_status.last_error);
                cJSON_AddBoolToObject(result, "wifi_ready",
                                      wifi_status.available);
                cJSON_AddBoolToObject(result, "wifi_desired",
                                      wifi_status.desired_connected);
            }
            if (wifi_service_get_scan_snapshot(&wifi_scan) == ESP_OK)
            {
                cJSON_AddNumberToObject(result, "wifi_scan_state",
                                        (double)wifi_scan.state);
                cJSON_AddNumberToObject(result, "wifi_scan_records",
                                        (double)wifi_scan.record_count);
                cJSON_AddNumberToObject(result, "wifi_scan_error",
                                        (double)wifi_scan.last_error);
            }
        }
    }
    else if (strcmp(method, "sim.sd") == 0)
    {
        const cJSON *action = params != NULL ?
                              cJSON_GetObjectItemCaseSensitive(params,
                                  "action") : NULL;
        const char *verb = cJSON_GetStringValue(action);
        if (verb != NULL && strcmp(verb, "mount") == 0)
        {
            *ok = host_sd_set_mounted(true) &&
                  _restart_recorder_service(true) == ESP_OK;
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error", "mount failed");
            }
        }
        else if (verb != NULL && strcmp(verb, "umount") == 0)
        {
            const bool removed = host_sd_set_mounted(false);
            _restart_recorder_service(false);
            *ok = removed && !host_sd_is_mounted();
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error", "umount failed");
            }
        }
        else if (verb != NULL && strcmp(verb, "write") == 0)
        {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(params,
                                "name");
            const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(params,
                                   "seconds");
            esp_err_t result_sd = ESP_ERR_INVALID_ARG;
            if (cJSON_IsString(name) && cJSON_IsNumber(seconds) &&
                    seconds->valuedouble >= 0.0 &&
                    seconds->valuedouble <= 3600.0)
            {
                result_sd = host_sd_write_wav(name->valuestring,
                                              (uint32_t)seconds->valuedouble);
                if (result_sd == ESP_OK)
                {
                    (void)_restart_recorder_service(host_sd_is_mounted());
                }
            }
            *ok = result_sd == ESP_OK;
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error",
                                        esp_err_to_name(result_sd));
            }
        }
        else if (verb != NULL && strcmp(verb, "svc") == 0)
        {
            recorder_service_snapshot_t snap;
            recorder_service_file_t files[16];
            size_t count = 0U;
            memset(&snap, 0, sizeof(snap));
            const esp_err_t snap_result = recorder_service_get_snapshot(&snap);
            const esp_err_t list_result = recorder_service_list(files, 16U,
                                          &count);
            cJSON_AddNumberToObject(result, "snap", (double)snap_result);
            cJSON_AddNumberToObject(result, "list", (double)list_result);
            cJSON_AddNumberToObject(result, "generation",
                                    (double)snap.generation);
            cJSON_AddNumberToObject(result, "state", (double)snap.state);
            cJSON_AddNumberToObject(result, "svc_files", (double)count);
            *ok = true;
        }
        else if (verb != NULL && strcmp(verb, "clear") == 0)
        {
            cJSON_AddNumberToObject(result, "removed",
                                    (double)host_sd_clear_recordings());
            (void)_restart_recorder_service(host_sd_is_mounted());
            *ok = true;
        }
        else if (verb != NULL && strcmp(verb, "list") == 0)
        {
            static char listed[24][SIM_SD_NAME_MAX];
            size_t listed_count = host_sd_list_recordings(listed, 24U);
            cJSON *files = cJSON_AddArrayToObject(result, "files");
            cJSON_AddBoolToObject(result, "mounted",
                                  host_sd_is_mounted());
            for (size_t index = 0U; index < listed_count &&
                    files != NULL; ++index)
            {
                cJSON_AddItemToArray(files, cJSON_CreateString(listed[index]));
            }
        }
        else
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid sd action");
        }
    }
    else if (strcmp(method, "sim.nvs") == 0)
    {
        const cJSON *action = params != NULL ?
                              cJSON_GetObjectItemCaseSensitive(params,
                                  "action") : NULL;
        const cJSON *key = params != NULL ?
                           cJSON_GetObjectItemCaseSensitive(params,
                               "key") : NULL;
        const char *verb = cJSON_GetStringValue(action);
        const char *text = cJSON_GetStringValue(key);
        if (verb != NULL && text != NULL && strcmp(verb, "set") == 0)
        {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(params,
                                 "value");
            *ok = cJSON_IsString(value) &&
                  nv_storage_set_str(text, value->valuestring) == ESP_OK;
        }
        else if (verb != NULL && text != NULL &&
                 strcmp(verb, "get") == 0)
        {
            char stored[128];
            size_t size = sizeof(stored);
            if (nv_storage_get_str(text, stored, &size) == ESP_OK)
            {
                cJSON_AddStringToObject(result, "value", stored);
            }
            else
            {
                *ok = false;
                cJSON_AddStringToObject(result, "error", "nvs key missing");
            }
        }
        else if (verb != NULL && text != NULL &&
                 strcmp(verb, "erase") == 0)
        {
            *ok = nv_storage_erase_key(text) == ESP_OK;
        }
        else
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid nvs params");
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
        const bool valid_endpoint = (path != NULL) &&
                                    ((strcmp(path, "location") == 0) ||
                                     (strcmp(path, "current") == 0) ||
                                     (strcmp(path, "alerts") == 0) ||
                                     (strcmp(path, "hourly") == 0) ||
                                     (strcmp(path, "daily") == 0));
        if (!valid_endpoint || (json == NULL) ||
                ((status != NULL) && !_number_in_range(status, 100.0, 599.0)))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid weather params");
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
                                         (status != NULL) ? (int)status->valuedouble : 200,
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
        esp_err_t res;
        if (!cJSON_IsBool(enabled))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid pause params");
        }
        else
        {
            res = cJSON_IsTrue(enabled) ? esp_lv_adapter_pause(-1)
                  : esp_lv_adapter_resume();
            *ok = (res == ESP_OK);
            if (!*ok)
            {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(res));
            }
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
        const bool valid_get = (want_get == NULL) || cJSON_IsBool(want_get);
        const bool valid_off = (off == NULL) ||
                               _number_in_range(off, INT32_MIN, INT32_MAX);
        const bool valid_standby = (standby == NULL) ||
                                   _number_in_range(standby, INT32_MIN, INT32_MAX);
        const bool want_read = cJSON_IsTrue(want_get);

        if (!valid_get || !valid_off || !valid_standby ||
                (!want_read && (off == NULL) && (standby == NULL)))
        {
            *ok = false;
            cJSON_AddStringToObject(result, "error", "invalid pm params");
        }
        else if (want_read)
        {
            *ok = true;
            cJSON_AddNumberToObject(result, "pm_state",
                                    (double)app_manager_pm_get_state());
            cJSON_AddNumberToObject(result, "off_ms",
                                    (double)app_manager_pm_get_timeout_ms());
            cJSON_AddNumberToObject(result, "standby_ms",
                                    (double)app_manager_pm_get_standby_delay_ms());
        }
        else
        {
            *ok = true;
        }
        if (*ok && (off != NULL))
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
        if (*ok && (standby != NULL))
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

static bool _write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0U;

    while (offset < length)
    {
        const ssize_t written = write(fd, data + offset, length - offset);
        if (written > 0)
        {
            offset += (size_t)written;
        }
        else if ((written < 0) && (errno == EINTR))
        {
            continue;
        }
        else
        {
            return false;
        }
    }
    return true;
}

static bool _write_line(int fd, const char *text)
{
    return _write_all(fd, text, strlen(text)) && _write_all(fd, "\n", 1U);
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
        bool sent = true;

        if (reply == NULL)
        {
            cJSON_Delete(request);
            continue;
        }
        if (request == NULL)
        {
            const char *err = "{\"ok\":false,\"error\":\"invalid json\"}";
            cJSON_Delete(reply);
            if (!_write_line(fd, err))
            {
                break;
            }
            continue;
        }
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(request, "id");
        const double id_value = cJSON_IsNumber(rid) ? rid->valuedouble : 0.0;
        cJSON_AddNumberToObject(reply, "id", id_value);
        (void)pthread_mutex_lock(&s_rpc_lock);
        result = _handle(request, &ok);
        (void)pthread_mutex_unlock(&s_rpc_lock);
        if (result == NULL)
        {
            ok = false;
        }
        if (!cJSON_IsNumber(rid))
        {
            cJSON_DeleteItemFromObject(reply, "id");
        }
        cJSON_AddBoolToObject(reply, "ok", ok);
        if (ok && (result != NULL))
        {
            cJSON_AddItemToObject(reply, "result", result);
        }
        else
        {
            cJSON *error = (result != NULL) ?
                           cJSON_DetachItemFromObjectCaseSensitive(result, "error") : NULL;
            if (!cJSON_IsString(error))
            {
                cJSON_Delete(error);
                error = cJSON_CreateString((result == NULL) ? "no memory" :
                                           "invalid params");
            }
            if (error != NULL)
            {
                cJSON_AddItemToObject(reply, "error", error);
            }
            cJSON_Delete(result);
        }
        text = cJSON_PrintUnformatted(reply);
        if (text != NULL)
        {
            sent = _write_line(fd, text);
            free(text);
        }
        cJSON_Delete(request);
        cJSON_Delete(reply);
        if (!sent)
        {
            break;
        }
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

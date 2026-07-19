#include "apps_integration_runtime.h"

#include "app_manager.h"
#include "app_manager_back_gesture.h"
#include "app_manager_builtin.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"
#include "app_theme.h"
#include "event_bus.h"
#include "host_wifi_service.h"
#include "power_service.h"
#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define UI_TIMEOUT_MS 1000U
#define WAIT_ATTEMPTS 2000U
#define BUILTIN_APP_COUNT 4U
#define LIFECYCLE_OBSERVATION_CAPACITY 1024U
#define LIFECYCLE_ID_BYTES 32U

EVENT_BUS_DEFINE_ID(CROSS_LAYER_TEST_MSG);

extern const app_manager_app_desc_t _app_manager_apps_start[];
extern const app_manager_app_desc_t _app_manager_apps_end[];
extern const app_manager_page_desc_t _app_manager_pages_start[];
extern const app_manager_page_desc_t _app_manager_pages_end[];

static atomic_uint s_noop_count;
static atomic_uint s_queued_scan_probe_count;

typedef struct lifecycle_observation
{
    char app_id[LIFECYCLE_ID_BYTES];
    char page_id[LIFECYCLE_ID_BYTES];
    app_manager_msg_type_t message;
    app_manager_lifecycle_observer_phase_t phase;
    size_t live_objects;
    size_t live_timers;
} lifecycle_observation_t;

typedef struct lv_resource_counts
{
    size_t objects;
    size_t screens;
    size_t timers;
} lv_resource_counts_t;

typedef struct first_frame_probe_config
{
    const char *app_id;
    const char *page_id;
    const char *expected_text;
    const char *forbidden_text;
} first_frame_probe_config_t;

typedef struct first_frame_probe
{
    bool armed;
    char app_id[LIFECYCLE_ID_BYTES];
    char page_id[LIFECYCLE_ID_BYTES];
    const char *expected_text;
    const char *forbidden_text;
    lv_obj_t *screen;
    uint64_t sequence;
    uint64_t mount_after_sequence;
    uint64_t resume_after_sequence;
    uint64_t load_start_sequence;
    uint64_t loaded_sequence;
    uint64_t completion_sequence;
    size_t mount_after_count;
    size_t resume_after_count;
    size_t load_start_count;
    size_t loaded_count;
    size_t completion_count;
    size_t load_start_timers;
    esp_err_t completion_result;
    bool load_start_active_target;
    bool loaded_active_target;
    bool expected_text_at_load_start;
    bool forbidden_text_at_load_start;
} first_frame_probe_t;

typedef struct queued_scan_pause_request
{
    wifi_service_scan_snapshot_t snapshot;
    event_bus_sub_handle_t probe_subscription;
} queued_scan_pause_request_t;

static lifecycle_observation_t
s_lifecycle_observations[LIFECYCLE_OBSERVATION_CAPACITY];
static size_t s_lifecycle_observation_count;
static first_frame_probe_t s_first_frame_probe;

static uint64_t _first_frame_next_sequence(void)
{
    return ++s_first_frame_probe.sequence;
}

static bool _first_frame_target_matches(const char *app_id,
                                        const char *page_id)
{
    return s_first_frame_probe.armed && strcmp(s_first_frame_probe.app_id,
            app_id) == 0 && strcmp(s_first_frame_probe.page_id, page_id) == 0;
}

static void _first_frame_screen_event(lv_event_t *event)
{
    lv_obj_t *screen = lv_event_get_target_obj(event);
    assert(s_first_frame_probe.armed);
    assert(screen == s_first_frame_probe.screen);

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SCREEN_LOAD_START)
    {
        ++s_first_frame_probe.load_start_count;
        if (s_first_frame_probe.load_start_sequence == 0U)
        {
            s_first_frame_probe.load_start_sequence =
                _first_frame_next_sequence();
        }
        s_first_frame_probe.load_start_active_target =
            lv_screen_active() == screen;
        s_first_frame_probe.load_start_timers = host_lv_live_timer_count();
        s_first_frame_probe.expected_text_at_load_start =
            host_lv_transition_target_has_text(
                s_first_frame_probe.expected_text);
        s_first_frame_probe.forbidden_text_at_load_start =
            host_lv_transition_target_has_text(
                s_first_frame_probe.forbidden_text);
    }
    else if (code == LV_EVENT_SCREEN_LOADED)
    {
        ++s_first_frame_probe.loaded_count;
        if (s_first_frame_probe.loaded_sequence == 0U)
        {
            s_first_frame_probe.loaded_sequence =
                _first_frame_next_sequence();
        }
        s_first_frame_probe.loaded_active_target = lv_screen_active() == screen;
    }
}

static void _first_frame_observe_lifecycle(
    const char *app_id, const char *page_id,
    app_manager_msg_type_t message,
    app_manager_lifecycle_observer_phase_t phase)
{
    if (!_first_frame_target_matches(app_id, page_id) ||
            phase != APP_MANAGER_LIFECYCLE_OBSERVER_AFTER)
    {
        return;
    }

    if (message == APP_MANAGER_MSG_ONMOUNT)
    {
        ++s_first_frame_probe.mount_after_count;
        if (s_first_frame_probe.mount_after_sequence == 0U)
        {
            s_first_frame_probe.mount_after_sequence =
                _first_frame_next_sequence();
            s_first_frame_probe.screen = app_manager_this_page_screen();
            assert(s_first_frame_probe.screen != NULL);
            assert(lv_obj_add_event_cb(
                       s_first_frame_probe.screen, _first_frame_screen_event,
                       LV_EVENT_SCREEN_LOAD_START, NULL) != NULL);
            assert(lv_obj_add_event_cb(
                       s_first_frame_probe.screen, _first_frame_screen_event,
                       LV_EVENT_SCREEN_LOADED, NULL) != NULL);
        }
    }
    else if (message == APP_MANAGER_MSG_ONRESUME)
    {
        ++s_first_frame_probe.resume_after_count;
        if (s_first_frame_probe.resume_after_sequence == 0U)
        {
            s_first_frame_probe.resume_after_sequence =
                _first_frame_next_sequence();
        }
    }
}

static void _sleep_one_ms(void)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static esp_err_t _ui_barrier(void *arg)
{
    (void)arg;
    return ESP_OK;
}

static esp_err_t _navigate(app_manager_nav_operation_t operation,
                           const char *app_id, const char *page_id)
{
    const app_manager_nav_request_t request =
    {
        .operation = operation,
        .app_id = app_id,
        .page_id = page_id,
    };
    return app_manager_navigate(&request, UI_TIMEOUT_MS);
}

static void _lifecycle_observer(
    const char *app_id, const char *page_id,
    app_manager_msg_type_t message,
    app_manager_lifecycle_observer_phase_t phase)
{
    assert(s_lifecycle_observation_count <
           LIFECYCLE_OBSERVATION_CAPACITY);
    lifecycle_observation_t *observation =
        &s_lifecycle_observations[s_lifecycle_observation_count++];
    (void)snprintf(observation->app_id, sizeof(observation->app_id),
                   "%s", app_id);
    (void)snprintf(observation->page_id, sizeof(observation->page_id),
                   "%s", page_id);
    observation->message = message;
    observation->phase = phase;
    observation->live_objects = host_lv_live_object_count();
    observation->live_timers = host_lv_live_timer_count();
    _first_frame_observe_lifecycle(app_id, page_id, message, phase);
}

static size_t _lifecycle_observed(const char *app_id, const char *page_id,
                                  app_manager_msg_type_t message,
                                  app_manager_lifecycle_observer_phase_t phase)
{
    size_t count = 0;
    for (size_t index = 0; index < s_lifecycle_observation_count; ++index)
    {
        const lifecycle_observation_t *observation =
            &s_lifecycle_observations[index];
        if (strcmp(observation->app_id, app_id) == 0 &&
                strcmp(observation->page_id, page_id) == 0 &&
                observation->message == message &&
                observation->phase == phase)
        {
            ++count;
        }
    }
    return count;
}

static esp_err_t _first_frame_probe_arm_on_ui(void *arg)
{
    const first_frame_probe_config_t *config = arg;
    assert(config != NULL);
    assert(config->app_id != NULL && config->page_id != NULL);
    assert(config->expected_text != NULL && config->forbidden_text != NULL);

    memset(&s_first_frame_probe, 0, sizeof(s_first_frame_probe));
    (void)snprintf(s_first_frame_probe.app_id,
                   sizeof(s_first_frame_probe.app_id), "%s", config->app_id);
    (void)snprintf(s_first_frame_probe.page_id,
                   sizeof(s_first_frame_probe.page_id), "%s", config->page_id);
    s_first_frame_probe.expected_text = config->expected_text;
    s_first_frame_probe.forbidden_text = config->forbidden_text;
    s_first_frame_probe.armed = true;
    return ESP_OK;
}

static esp_err_t _first_frame_probe_snapshot_on_ui(void *arg)
{
    first_frame_probe_t *snapshot = arg;
    assert(snapshot != NULL);
    *snapshot = s_first_frame_probe;
    return ESP_OK;
}

static first_frame_probe_t _first_frame_probe_snapshot(void)
{
    first_frame_probe_t snapshot;
    assert(app_manager_ui_call(_first_frame_probe_snapshot_on_ui, &snapshot,
                               UI_TIMEOUT_MS) == ESP_OK);
    return snapshot;
}

static void _first_frame_navigation_completed(esp_err_t result, void *context)
{
    first_frame_probe_t *probe = context;
    assert(probe == &s_first_frame_probe);
    assert(probe->armed);
    ++probe->completion_count;
    if (probe->completion_sequence == 0U)
    {
        probe->completion_sequence = _first_frame_next_sequence();
    }
    probe->completion_result = result;
}

static void _start_first_frame_navigation(
    app_manager_nav_operation_t operation, const char *app_id,
    const char *page_id, const char *expected_text,
    const char *forbidden_text)
{
    const first_frame_probe_config_t config =
    {
        .app_id = app_id,
        .page_id = page_id,
        .expected_text = expected_text,
        .forbidden_text = forbidden_text,
    };
    assert(app_manager_ui_call(_first_frame_probe_arm_on_ui, (void *)&config,
                               UI_TIMEOUT_MS) == ESP_OK);

    const app_manager_nav_request_t request =
    {
        .operation = operation,
        .app_id = app_id,
        .page_id = page_id,
    };
    assert(app_manager_navigate_async(
               &request, _first_frame_navigation_completed,
               &s_first_frame_probe) == ESP_OK);
}

static bool _wait_for_first_frame_completion(void)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        const first_frame_probe_t snapshot = _first_frame_probe_snapshot();
        if (snapshot.completion_count > 0U)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static void _assert_first_frame_probe(size_t expected_timers)
{
    const first_frame_probe_t probe = _first_frame_probe_snapshot();
    assert(probe.armed);
    assert(probe.completion_result == ESP_OK);
    assert(probe.mount_after_count == 1U);
    assert(probe.resume_after_count == 1U);
    assert(probe.load_start_count == 1U);
    assert(probe.loaded_count == 1U);
    assert(probe.completion_count == 1U);
    assert(probe.mount_after_sequence > 0U);
    assert(probe.mount_after_sequence < probe.resume_after_sequence);
    assert(probe.resume_after_sequence < probe.load_start_sequence);
    assert(probe.load_start_sequence < probe.loaded_sequence);
    assert(probe.loaded_sequence < probe.completion_sequence);
    assert(probe.load_start_active_target);
    assert(probe.loaded_active_target);
    assert(probe.expected_text_at_load_start);
    assert(!probe.forbidden_text_at_load_start);
    assert(probe.load_start_timers == expected_timers);
}

static esp_err_t _query_lv_resources_on_ui(void *arg)
{
    lv_resource_counts_t *counts = arg;
    counts->objects = host_lv_live_object_count();
    counts->screens = host_lv_live_screen_count();
    counts->timers = host_lv_live_timer_count();
    return ESP_OK;
}

static lv_resource_counts_t _lv_resource_counts(void)
{
    lv_resource_counts_t counts = {0};
    assert(app_manager_ui_call(_query_lv_resources_on_ui, &counts,
                               UI_TIMEOUT_MS) == ESP_OK);
    return counts;
}

static esp_err_t _screen_pause_on_ui(void *arg)
{
    (void)arg;
    app_manager_back_gesture_screen_suspend();
    return app_manager_lifecycle_screen_pause();
}

static esp_err_t _screen_resume_on_ui(void *arg)
{
    (void)arg;
    esp_err_t result = app_manager_lifecycle_screen_resume();
    if (result == ESP_OK)
    {
        app_manager_back_gesture_screen_resume();
    }
    return result;
}

static void _noop_callback(void *arg)
{
    (void)arg;
    atomic_fetch_add(&s_noop_count, 1U);
}

static void _subscription_probe_callback(event_bus_msg_id_t msg_id,
        uint32_t sub_type, const void *payload, size_t payload_size,
        void *user_data)
{
    (void)msg_id;
    (void)sub_type;
    (void)payload;
    (void)payload_size;
    (void)user_data;
}

static void _queued_scan_probe_callback(event_bus_msg_id_t msg_id,
                                        uint32_t sub_type, const void *payload,
                                        size_t payload_size, void *user_data)
{
    (void)user_data;
    assert(msg_id == WIFI_SERVICE_MSG);
    assert(sub_type == WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT);
    assert(payload != NULL);
    assert(payload_size == sizeof(wifi_service_scan_snapshot_t));
    atomic_fetch_add(&s_queued_scan_probe_count, 1U);
}

static esp_err_t _publish_scan_and_pause_on_ui(void *arg)
{
    queued_scan_pause_request_t *request = arg;
    esp_err_t result = host_wifi_service_publish_raw_scan(
                           &request->snapshot, sizeof(request->snapshot));
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = event_bus_unsubscribe(request->probe_subscription);
    if (result != ESP_OK)
    {
        goto exit;
    }
    request->probe_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    app_manager_back_gesture_screen_suspend();
    result = app_manager_lifecycle_screen_pause();

exit:
    return result;
}

static esp_err_t _click_action_on_ui(void *arg)
{
    return host_lv_click_action(arg) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _click_back_on_ui(void *arg)
{
    (void)arg;
    return host_lv_click_back() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

typedef struct text_query
{
    const char *text;
    bool found;
} text_query_t;

typedef enum touch_action
{
    TOUCH_ACTION_PRESS = 0,
    TOUCH_ACTION_MOVE,
    TOUCH_ACTION_RELEASE,
    TOUCH_ACTION_RESET,
} touch_action_t;

typedef struct touch_request
{
    touch_action_t action;
    int32_t x;
    int32_t y;
    bool handled;
} touch_request_t;

typedef struct system_gesture_snapshot
{
    size_t object_count;
    size_t visible_edge_count;
    bool left_edge_found;
    bool right_edge_found;
    bool indicator_found;
    bool indicator_visible;
    bool arrow_found;
    bool arrow_visible;
    bool pointer_target_found;
    bool active_screen_found;
    host_lv_system_object_snapshot_t left_edge;
    host_lv_system_object_snapshot_t right_edge;
    host_lv_system_object_snapshot_t indicator;
    host_lv_system_object_snapshot_t arrow;
    host_lv_system_object_snapshot_t pointer_target;
    host_lv_system_object_snapshot_t active_screen;
} system_gesture_snapshot_t;

#define GESTURE_EDGE_WIDTH         29
#define GESTURE_TRIGGER_DISTANCE   55
#define GESTURE_DIRECTION_SLOP     11
#define GESTURE_CANVAS_WIDTH       64
#define GESTURE_CANVAS_HEIGHT      144
#define GESTURE_CURVE_SAMPLE_COUNT 5U

typedef struct gesture_curve_snapshot
{
    const lv_obj_t *canvas;
    bool valid;
    lv_opa_t alpha[GESTURE_CURVE_SAMPLE_COUNT][GESTURE_CANVAS_WIDTH];
} gesture_curve_snapshot_t;

static const int32_t s_gesture_curve_sample_y[GESTURE_CURVE_SAMPLE_COUNT] =
{
    0, 36, 71, 107, 143,
};

static esp_err_t _query_text_on_ui(void *arg)
{
    text_query_t *query = arg;
    query->found = host_lv_has_text(query->text);
    return ESP_OK;
}

static esp_err_t _query_transition_target_text_on_ui(void *arg)
{
    text_query_t *query = arg;
    query->found = host_lv_transition_target_has_text(query->text);
    return ESP_OK;
}

static esp_err_t _touch_on_ui(void *arg)
{
    touch_request_t *request = arg;
    if (request->action == TOUCH_ACTION_PRESS)
    {
        request->handled = host_lv_touch_press(request->x, request->y);
    }
    else if (request->action == TOUCH_ACTION_MOVE)
    {
        request->handled = host_lv_touch_move(request->x, request->y);
    }
    else if (request->action == TOUCH_ACTION_RELEASE)
    {
        request->handled = host_lv_touch_release(request->x, request->y);
    }
    else
    {
        host_lv_touch_reset();
        request->handled = true;
    }
    return ESP_OK;
}

static esp_err_t _query_system_gesture_on_ui(void *arg)
{
    system_gesture_snapshot_t *state = arg;
    memset(state, 0, sizeof(*state));
    state->object_count = host_lv_system_object_count();
    for (size_t index = 0; index < state->object_count; ++index)
    {
        host_lv_system_object_snapshot_t object;
        assert(host_lv_system_object_snapshot(index, &object));
        if (object.text != NULL && strcmp(object.text, LV_SYMBOL_LEFT) == 0)
        {
            state->arrow_found = true;
            state->arrow_visible = object.visible;
            state->arrow = object;
        }
        else if (object.width == GESTURE_EDGE_WIDTH && object.height == 448)
        {
            if (object.x == 0)
            {
                state->left_edge_found = true;
                state->left_edge = object;
            }
            else
            {
                state->right_edge_found = true;
                state->right_edge = object;
            }
            if (object.visible &&
                    (object.flags & LV_OBJ_FLAG_CLICKABLE) != 0U)
            {
                ++state->visible_edge_count;
            }
        }
        else if (object.canvas && object.width == GESTURE_CANVAS_WIDTH &&
                 object.height == GESTURE_CANVAS_HEIGHT)
        {
            state->indicator_found = true;
            state->indicator_visible = object.visible;
            state->indicator = object;
        }
    }
    state->pointer_target_found = host_lv_pointer_target_snapshot(
                                      &state->pointer_target);
    state->active_screen_found = host_lv_active_screen_snapshot(
                                     &state->active_screen);
    return ESP_OK;
}

static esp_err_t _query_gesture_curve_on_ui(void *arg)
{
    gesture_curve_snapshot_t *snapshot = arg;
    snapshot->valid = true;
    for (size_t row = 0U; row < GESTURE_CURVE_SAMPLE_COUNT; ++row)
    {
        for (int32_t x = 0; x < GESTURE_CANVAS_WIDTH; ++x)
        {
            if (!host_lv_canvas_alpha_snapshot(
                        snapshot->canvas, x, s_gesture_curve_sample_y[row],
                        &snapshot->alpha[row][x]))
            {
                snapshot->valid = false;
                return ESP_OK;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t _presentation_init_on_ui(void *arg)
{
    (void)arg;
    return app_manager_presentation_init();
}

static esp_err_t _back_gesture_init_on_ui(void *arg)
{
    (void)arg;
    return app_manager_back_gesture_init(lv_display_get_default(),
                                         host_lv_pointer_indev());
}

static esp_err_t _runtime_deinit_on_ui(void *arg)
{
    (void)arg;
    esp_err_t result = app_manager_lifecycle_deinit();
    if (result == ESP_OK)
    {
        result = app_manager_back_gesture_deinit();
    }
    if (result == ESP_OK)
    {
        result = app_manager_presentation_deinit();
    }
    return result;
}

static bool _ui_has_text(const char *text)
{
    text_query_t query = {.text = text};
    assert(app_manager_ui_call(_query_text_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    return query.found;
}

static bool _transition_target_has_text(const char *text)
{
    text_query_t query = {.text = text};
    assert(app_manager_ui_call(_query_transition_target_text_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    return query.found;
}

static bool _touch(touch_action_t action, int32_t x, int32_t y)
{
    touch_request_t request =
    {
        .action = action,
        .x = x,
        .y = y,
    };
    assert(app_manager_ui_call(_touch_on_ui, &request,
                               UI_TIMEOUT_MS) == ESP_OK);
    return request.handled;
}

static system_gesture_snapshot_t _system_gesture_snapshot(void)
{
    system_gesture_snapshot_t state;
    assert(app_manager_ui_call(_query_system_gesture_on_ui, &state,
                               UI_TIMEOUT_MS) == ESP_OK);
    return state;
}

static gesture_curve_snapshot_t _gesture_curve_snapshot(
    const lv_obj_t *canvas)
{
    gesture_curve_snapshot_t snapshot = {.canvas = canvas};
    assert(app_manager_ui_call(_query_gesture_curve_on_ui, &snapshot,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(snapshot.valid);
    return snapshot;
}

static size_t _gesture_curve_coverage(const lv_opa_t *row)
{
    size_t coverage = GESTURE_CANVAS_WIDTH;
    while (coverage > 0U && row[coverage - 1U] == LV_OPA_TRANSP)
    {
        --coverage;
    }
    return coverage;
}

static void _assert_left_gesture_curve(
    const gesture_curve_snapshot_t *curve)
{
    static const size_t expected_coverage[GESTURE_CURVE_SAMPLE_COUNT] =
    {
        28U, 46U, 62U, 46U, 28U,
    };

    for (size_t row = 0U; row < GESTURE_CURVE_SAMPLE_COUNT; ++row)
    {
        assert(_gesture_curve_coverage(curve->alpha[row]) ==
               expected_coverage[row]);
        assert(curve->alpha[row][0] == LV_OPA_COVER);
    }
    assert(memcmp(curve->alpha[0], curve->alpha[4],
                  GESTURE_CANVAS_WIDTH) == 0);
    assert(memcmp(curve->alpha[1], curve->alpha[3],
                  GESTURE_CANVAS_WIDTH) == 0);
    assert(curve->alpha[1][44] == LV_OPA_COVER);
    assert(curve->alpha[1][45] == 46);
    assert(curve->alpha[1][46] == LV_OPA_TRANSP);
    assert(curve->alpha[2][60] == LV_OPA_COVER);
    assert(curve->alpha[2][61] == 253);
    assert(curve->alpha[2][62] == LV_OPA_TRANSP);
}

static void _assert_mirrored_gesture_curve(
    const gesture_curve_snapshot_t *left,
    const gesture_curve_snapshot_t *right)
{
    for (size_t row = 0U; row < GESTURE_CURVE_SAMPLE_COUNT; ++row)
    {
        for (size_t x = 0U; x < GESTURE_CANVAS_WIDTH; ++x)
        {
            assert(right->alpha[row][x] ==
                   left->alpha[row][GESTURE_CANVAS_WIDTH - x - 1U]);
        }
    }
}

static void _assert_same_screen(
    const host_lv_system_object_snapshot_t *expected,
    const host_lv_system_object_snapshot_t *actual)
{
    assert(expected->object == actual->object);
    assert(expected->x == actual->x);
    assert(expected->y == actual->y);
    assert(expected->width == actual->width);
    assert(expected->height == actual->height);
    assert(expected->opacity == actual->opacity);
}

static void _assert_indicator_hidden(void)
{
    const system_gesture_snapshot_t system = _system_gesture_snapshot();
    assert(system.indicator_found);
    assert(!system.indicator_visible);
    assert(system.arrow_found);
    assert(!system.arrow_visible);
}

static void _click_action(const char *title)
{
    assert(app_manager_ui_call(_click_action_on_ui, (void *)title,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static void _click_back(void)
{
    assert(app_manager_ui_call(_click_back_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static bool _wait_for_text(const char *text)
{
    bool found = false;
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS && !found; ++attempt)
    {
        found = _ui_has_text(text);
        if (!found)
        {
            _sleep_one_ms();
        }
    }
    return found;
}

static bool _wait_for_active(const char *app_id)
{
    bool active = false;
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS && !active; ++attempt)
    {
        active = app_manager_is_actived(app_id);
        if (!active)
        {
            _sleep_one_ms();
        }
    }
    return active;
}

static bool _wait_for_page_active(const char *app_id, const char *page_id)
{
    bool active = false;
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS && !active; ++attempt)
    {
        active = app_page_is_actived(app_id, page_id);
        if (!active)
        {
            _sleep_one_ms();
        }
    }
    return active;
}

static bool _wait_for_transitioning(void)
{
    bool transitioning = false;
    for (unsigned attempt = 0;
            attempt < WAIT_ATTEMPTS && !transitioning; ++attempt)
    {
        transitioning = app_manager_is_transitioning();
        if (!transitioning)
        {
            _sleep_one_ms();
        }
    }
    return transitioning;
}

static void _assert_event_slot_headroom(size_t occupied)
{
    event_bus_sub_handle_t handles[EVENT_BUS_MAX_SUBSCRIBERS];
    assert(occupied <= EVENT_BUS_MAX_SUBSCRIBERS);
    const size_t available = EVENT_BUS_MAX_SUBSCRIBERS - occupied;
    for (size_t index = 0; index < available; ++index)
    {
        assert(event_bus_subscribe(CROSS_LAYER_TEST_MSG,
                                   EVENT_BUS_SUB_TYPE_ANY,
                                   _subscription_probe_callback, NULL,
                                   EVENT_BUS_DISPATCH_UI,
                                   &handles[index]) == ESP_OK);
    }
    event_bus_sub_handle_t overflow = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(CROSS_LAYER_TEST_MSG,
                               EVENT_BUS_SUB_TYPE_ANY,
                               _subscription_probe_callback, NULL,
                               EVENT_BUS_DISPATCH_UI,
                               &overflow) == ESP_ERR_NO_MEM);
    for (size_t index = 0; index < available; ++index)
    {
        assert(event_bus_unsubscribe(handles[index]) == ESP_OK);
    }
}

static void _initialize_stack(void)
{
    host_runtime_reset();
    assert(!app_manager_back_gesture_is_enabled());
    assert(app_theme_init() == ESP_OK);
    for (int id = 0; id < APP_THEME_FONT_MAX; ++id)
    {
        assert(app_theme_set_font((app_theme_font_id_t)id,
                                  LV_FONT_DEFAULT) == ESP_OK);
    }
    assert(app_manager_mailbox_init() == ESP_OK);
    assert(event_bus_init() == ESP_OK);

    app_manager_ui_dispatch_fn dispatcher = NULL;
    assert(app_manager_get_ui_dispatch_fn(&dispatcher) == ESP_OK);
    assert(dispatcher != NULL);
    assert(event_bus_register_ui_dispatch(dispatcher) == ESP_OK);

    const ptrdiff_t app_descriptor_count =
        _app_manager_apps_end - _app_manager_apps_start;
    const ptrdiff_t page_descriptor_count =
        _app_manager_pages_end - _app_manager_pages_start;
    assert(app_descriptor_count == (ptrdiff_t)BUILTIN_APP_COUNT);
    assert(page_descriptor_count >= (ptrdiff_t)BUILTIN_APP_COUNT);
    assert(app_manager_register_builtin_descriptors(
               _app_manager_apps_start, (size_t)app_descriptor_count,
               _app_manager_pages_start,
               (size_t)page_descriptor_count) == ESP_OK);
    assert(app_manager_builtin_discover() == (int)BUILTIN_APP_COUNT);
    assert(app_manager_ui_call(_presentation_init_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_lifecycle_configure(
               0, APP_MANAGER_RESIDENT_REJECT,
               NULL, NULL, NULL, NULL) == ESP_OK);
    assert(app_manager_lifecycle_init(0) == ESP_OK);
    assert(app_manager_navigation_init() == ESP_OK);
    assert(app_manager_ui_call(_back_gesture_init_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    memset(s_lifecycle_observations, 0,
           sizeof(s_lifecycle_observations));
    s_lifecycle_observation_count = 0;
    app_manager_lifecycle_host_set_observer(_lifecycle_observer);
}

static void _test_real_app_navigation(void)
{
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    assert(_ui_has_text("08:30"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(_lv_resource_counts().screens == 2U);

    power_service_snapshot_t snapshot =
    {
        .info = {
            .battery_voltage_mv = 4020,
            .battery_percent = 90,
            .is_charging = true,
            .is_vbus_connected = true,
        },
        .sampled_at_ms = 234567,
        .valid = true,
    };
    assert(event_bus_publish(
               POWER_SERVICE_MSG,
               POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE,
               &snapshot, sizeof(snapshot),
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("90% | Charging"));

    _click_action("Applications");
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_MENU, "root"));
    /* Home must cancel its power subscription as part of real ONPAUSE. */
    _assert_event_slot_headroom(0);

    _click_action("Settings");
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "root"));

    _start_first_frame_navigation(
        APP_MANAGER_NAV_OP_OPEN_PAGE, APP_MANAGER_ID_SETTINGS, "power",
        "3910 mV", "Reading...");
    assert(_wait_for_transitioning());
    assert(_transition_target_has_text("3910 mV"));
    assert(!_transition_target_has_text("Reading..."));
    assert(_lv_resource_counts().timers == 0U);
    _assert_event_slot_headroom(1);
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "power"));
    assert(_wait_for_first_frame_completion());
    _assert_first_frame_probe(0U);
    assert(_ui_has_text("3910 mV"));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "power"));

    _click_action("About");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "about"));
    assert(_ui_has_text("Compact ESP32-S3 wearable platform"));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "about"));

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_MENU, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETUP, "root"));
    assert(host_wifi_service_current_session() != 0);
    assert(host_wifi_service_current_operation() != 0);

    wifi_service_scan_snapshot_t scan =
    {
        .session_id = host_wifi_service_current_session(),
        .operation_id = host_wifi_service_current_operation(),
        .state = WIFI_SERVICE_SCAN_RESULTS,
        .last_error = ESP_OK,
        .record_count = 1,
    };
    memcpy(scan.records[0].ssid, "Cross Layer AP",
           sizeof("Cross Layer AP"));
    scan.records[0].rssi = -45;
    scan.records[0].channel = 6;
    scan.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    assert(host_wifi_service_publish_scan(&scan) == ESP_OK);
    assert(_wait_for_text("Select a network"));
    _click_action("Cross Layer AP");
    wifi_service_status_snapshot_t ready =
    {
        .session_id = host_wifi_service_current_session(),
        .operation_id = host_wifi_service_current_operation(),
        .state = WIFI_SERVICE_STATE_IP_READY,
        .last_error = ESP_OK,
        .ipv4_address = UINT32_C(0x0100000a),
        .available = true,
        .desired_connected = true,
    };
    memcpy(ready.ssid, "Cross Layer AP", sizeof("Cross Layer AP"));
    assert(host_wifi_service_publish_status(&ready) == ESP_OK);
    assert(_wait_for_text("Connected"));

    const unsigned disconnects = host_wifi_service_call_count(
                                     HOST_WIFI_SERVICE_CALL_REQUEST_DISCONNECT);
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(host_wifi_service_call_count(
               HOST_WIFI_SERVICE_CALL_REQUEST_DISCONNECT) == disconnects);
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
}

static void _test_home_resume_before_first_draw(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    _click_action("Applications");
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    _start_first_frame_navigation(
        APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, "root", "08:30", "--:--");
    assert(_wait_for_transitioning());
    assert(_transition_target_has_text("08:30"));
    assert(!_transition_target_has_text("--:--"));
    assert(_lv_resource_counts().timers == 1U);
    _assert_event_slot_headroom(1);

    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(_wait_for_first_frame_completion());
    _assert_first_frame_probe(1U);
    assert(_ui_has_text("08:30"));
    assert(_lv_resource_counts().timers == 1U);
    _assert_event_slot_headroom(1);
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(app_manager_get_running_apps() == 1U);
}

static void _test_latest_power_backpressure(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    atomic_store(&s_noop_count, 0U);
    app_manager_mailbox_host_timer_pause(true);

    power_service_snapshot_t snapshot =
    {
        .info =
        {
            .battery_voltage_mv = 3900,
            .battery_percent = 0,
            .is_charging = false,
            .is_vbus_connected = false,
        },
        .sampled_at_ms = 0,
        .valid = true,
    };
    for (unsigned index = 0; index < 1000U; ++index)
    {
        snapshot.info.battery_percent = (int8_t)(index % 100U);
        snapshot.sampled_at_ms = (int64_t)index;
        assert(event_bus_publish(
                   POWER_SERVICE_MSG,
                   POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE,
                   &snapshot, sizeof(snapshot),
                   EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    }

    for (unsigned index = 0; index < 23U; ++index)
    {
        assert(app_manager_ui_post(_noop_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_noop_count) == 0U);

    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS &&
            atomic_load(&s_noop_count) != 23U; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load(&s_noop_count) == 23U);
    app_manager_mailbox_host_timer_pause(false);
    assert(_ui_has_text("99% | On battery"));
}

static esp_err_t _publish_status_and_exit_setup_on_ui(void *arg)
{
    const wifi_service_status_snapshot_t *snapshot = arg;
    esp_err_t result = event_bus_publish(
                           WIFI_SERVICE_MSG,
                           WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           snapshot, sizeof(*snapshot),
                           EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    if (result != ESP_OK)
    {
        goto exit;
    }
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT,
        .app_id = APP_MANAGER_ID_SETUP,
    };
    result = app_manager_navigate_async(&request, NULL, NULL);

exit:
    return result;
}

static void _test_latest_wifi_backpressure_and_reopen(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(_wait_for_text("Connected"));
    _click_action("Scan networks");

    const wifi_service_session_id_t old_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t old_operation =
        host_wifi_service_current_operation();
    assert(old_session != 0 && old_operation != 0);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    atomic_store(&s_noop_count, 0U);
    app_manager_mailbox_host_timer_pause(true);
    wifi_service_status_snapshot_t status =
    {
        .state = WIFI_SERVICE_STATE_IDLE,
        .last_error = ESP_OK,
        .available = true,
    };
    wifi_service_scan_snapshot_t scan =
    {
        .session_id = old_session,
        .operation_id = old_operation,
        .state = WIFI_SERVICE_SCAN_RESULTS,
        .last_error = ESP_OK,
        .record_count = 1,
    };
    scan.records[0].rssi = -50;
    scan.records[0].channel = 6;
    scan.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    for (unsigned index = 0; index < 1000U; ++index)
    {
        status.last_error = (int32_t)index;
        (void)snprintf(scan.records[0].ssid,
                       sizeof(scan.records[0].ssid),
                       "WiFi %03u", index);
        assert(host_wifi_service_publish_status(&status) == ESP_OK);
        assert(host_wifi_service_publish_scan(&scan) == ESP_OK);
    }

    for (unsigned index = 0; index < 22U; ++index)
    {
        assert(app_manager_ui_post(_noop_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_noop_count) == 0U);
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS &&
            atomic_load(&s_noop_count) != 22U; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load(&s_noop_count) == 22U);
    app_manager_mailbox_host_timer_pause(false);
    assert(_ui_has_text("WiFi 999"));

    _click_action("WiFi 999");
    const wifi_service_operation_id_t connect_operation =
        host_wifi_service_current_operation();
    assert(connect_operation != 0);
    wifi_service_status_snapshot_t queued =
    {
        .generation = UINT64_C(100000),
        .session_id = old_session,
        .operation_id = connect_operation,
        .state = WIFI_SERVICE_STATE_CONNECTING,
        .last_error = ESP_OK,
        .available = true,
        .desired_connected = true,
    };
    memcpy(queued.ssid, "WiFi 999", sizeof("WiFi 999"));
    assert(app_manager_ui_call(_publish_status_and_exit_setup_on_ui, &queued,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    _assert_event_slot_headroom(1);

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    const wifi_service_session_id_t new_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t new_operation =
        host_wifi_service_current_operation();
    assert(new_session != 0 && new_session != old_session);
    assert(new_operation != 0);

    wifi_service_scan_snapshot_t stale =
    {
        .generation = UINT64_C(200000),
        .session_id = old_session,
        .operation_id = old_operation,
        .state = WIFI_SERVICE_SCAN_RESULTS,
        .last_error = ESP_OK,
        .record_count = 1,
    };
    memcpy(stale.records[0].ssid, "Old Session AP",
           sizeof("Old Session AP"));
    stale.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    assert(host_wifi_service_publish_raw_scan(&stale, sizeof(stale)) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(!_ui_has_text("Old Session AP"));
    assert(_ui_has_text("Scanning..."));

    scan.session_id = new_session;
    scan.operation_id = new_operation;
    memcpy(scan.records[0].ssid, "Current Session AP",
           sizeof("Current Session AP"));
    assert(host_wifi_service_publish_scan(&scan) == ESP_OK);
    assert(_wait_for_text("Current Session AP"));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    _assert_event_slot_headroom(1);
}

static void _assert_real_page_start_contract(void)
{
    static const struct
    {
        const char *app_id;
        const char *page_id;
    } expected_pages[] =
    {
        {APP_MANAGER_ID_HOME, "root"},
        {APP_MANAGER_ID_MENU, "root"},
        {APP_MANAGER_ID_SETTINGS, "root"},
        {APP_MANAGER_ID_SETTINGS, "power"},
        {APP_MANAGER_ID_SETTINGS, "about"},
        {APP_MANAGER_ID_SETUP, "root"},
    };

    for (size_t page = 0;
            page < sizeof(expected_pages) / sizeof(expected_pages[0]); ++page)
    {
        size_t starts_before = 0;
        size_t starts = 0;
        size_t objects_before = 0;
        size_t timers_before = 0;
        bool awaiting_after = false;
        for (size_t index = 0; index < s_lifecycle_observation_count; ++index)
        {
            const lifecycle_observation_t *observation =
                &s_lifecycle_observations[index];
            if (strcmp(observation->app_id,
                       expected_pages[page].app_id) == 0 &&
                    strcmp(observation->page_id,
                           expected_pages[page].page_id) == 0 &&
                    observation->message == APP_MANAGER_MSG_ONSTART)
            {
                if (observation->phase ==
                        APP_MANAGER_LIFECYCLE_OBSERVER_BEFORE)
                {
                    assert(!awaiting_after);
                    objects_before = observation->live_objects;
                    timers_before = observation->live_timers;
                    awaiting_after = true;
                    ++starts_before;
                }
                else
                {
                    assert(awaiting_after);
                    assert(observation->live_objects == objects_before);
                    assert(observation->live_timers == timers_before);
                    awaiting_after = false;
                    ++starts;
                }
            }
        }
        assert(!awaiting_after);
        assert(starts > 0);
        assert(starts == starts_before);
    }
}

static void _exercise_real_page_screen_lifecycle(
    const char *app_id, const char *page_id, const char *visible_text,
    size_t active_subscriptions)
{
    assert(app_manager_is_actived(app_id));
    assert(app_page_is_actived(app_id, page_id));
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(active_subscriptions);

    const size_t starts_before = _lifecycle_observed(
                                     app_id, page_id,
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      app_id, page_id,
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t pauses_before = _lifecycle_observed(
                                     app_id, page_id,
                                     APP_MANAGER_MSG_ONPAUSE,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    resources = _lv_resource_counts();
    assert(resources.objects == 0);
    assert(resources.screens == 1U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(app_id));
    assert(!app_page_is_actived(app_id, page_id));
    assert(app_manager_get_active_app_id() == NULL);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(app_id));
    assert(app_page_is_actived(app_id, page_id));
    assert(app_manager_is_page_present(app_id, page_id));
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.screens == 2U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(active_subscriptions);
    assert(_ui_has_text(visible_text));

    assert(_lifecycle_observed(
               app_id, page_id, APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               app_id, page_id, APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);
    assert(_lifecycle_observed(
               app_id, page_id, APP_MANAGER_MSG_ONPAUSE,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == pauses_before + 1U);
}

static void _test_other_real_app_screen_lifecycles(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_manager_get_running_apps() == 1U);

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_MENU, "root", "Applications", 0);

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "root", "Settings", 0);

    _click_action("Power");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "power"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "power", "3910 mV", 1);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));

    _click_action("About");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "about"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "about",
        "Compact ESP32-S3 wearable platform", 0);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));

    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(app_manager_get_running_apps() == 1U);
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(_ui_has_text("08:30"));
    _assert_event_slot_headroom(1);
}

static void _test_screen_pause_finishes_transition(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));

    _click_action("Applications");
    assert(_wait_for_transitioning());
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(!app_manager_is_transitioning());

    const lv_resource_counts_t paused = _lv_resource_counts();
    assert(paused.objects == 0U);
    assert(paused.screens == 1U);
    assert(paused.timers == 0U);
    _assert_event_slot_headroom(0);
    assert(app_manager_get_running_apps() == 2U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(APP_MANAGER_ID_MENU));
    assert(app_page_is_actived(APP_MANAGER_ID_MENU, "root"));
    assert(_ui_has_text("Applications"));
    assert(_lv_resource_counts().screens == 2U);

    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
}

static void _test_home_screen_lifecycle(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_running_apps() == 1U);
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 1U);
    _assert_event_slot_headroom(1);

    const size_t starts_before = _lifecycle_observed(
                                     APP_MANAGER_ID_HOME, "root",
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      APP_MANAGER_ID_HOME, "root",
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t pauses_before = _lifecycle_observed(
                                     APP_MANAGER_ID_HOME, "root",
                                     APP_MANAGER_MSG_ONPAUSE,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    resources = _lv_resource_counts();
    assert(resources.objects == 0);
    assert(resources.screens == 1U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(!app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_active_app_id() == NULL);
    assert(app_manager_get_running_apps() == 1U);
    assert(!app_manager_is_all_closed());

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_HOME, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(_navigate(APP_MANAGER_NAV_OP_OPEN_PAGE, APP_MANAGER_ID_SETTINGS,
                     "about") == ESP_ERR_INVALID_STATE);
    assert(_navigate(APP_MANAGER_NAV_OP_BACK, NULL, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(_navigate(APP_MANAGER_NAV_OP_BACK_TO, APP_MANAGER_ID_HOME,
                     "root") == ESP_ERR_INVALID_STATE);
    assert(_navigate(APP_MANAGER_NAV_OP_REMOVE_PAGE, APP_MANAGER_ID_HOME,
                     "root") == ESP_ERR_INVALID_STATE);
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT_SELF, NULL, NULL) ==
           ESP_ERR_INVALID_STATE);
    assert(app_manager_get_running_apps() == 1U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_running_apps() == 1U);
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.screens == 2U);
    assert(resources.timers == 1U);
    _assert_event_slot_headroom(1);
    assert(_ui_has_text("08:30"));

    assert(_lifecycle_observed(
               APP_MANAGER_ID_HOME, "root", APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_HOME, "root", APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_HOME, "root", APP_MANAGER_MSG_ONPAUSE,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == pauses_before + 1U);
}

static void _test_setup_screen_lifecycle(void)
{
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(_wait_for_text("Scanning..."));

    const wifi_service_session_id_t old_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t old_operation =
        host_wifi_service_current_operation();
    assert(old_session != 0 && old_operation != 0);
    const size_t starts_before = _lifecycle_observed(
                                     APP_MANAGER_ID_SETUP, "root",
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      APP_MANAGER_ID_SETUP, "root",
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    queued_scan_pause_request_t queued_pause =
    {
        .snapshot = {
            .generation = UINT64_C(300000),
            .session_id = old_session,
            .operation_id = old_operation,
            .state = WIFI_SERVICE_SCAN_RESULTS,
            .last_error = ESP_OK,
            .record_count = 1,
        },
        .probe_subscription = EVENT_BUS_SUB_HANDLE_INVALID,
    };
    memcpy(queued_pause.snapshot.records[0].ssid, "Paused Session AP",
           sizeof("Paused Session AP"));
    queued_pause.snapshot.records[0].security = WIFI_SERVICE_SECURITY_OPEN;
    atomic_store(&s_queued_scan_probe_count, 0U);
    assert(event_bus_subscribe(
               WIFI_SERVICE_MSG,
               WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
               _queued_scan_probe_callback, NULL, EVENT_BUS_DISPATCH_UI,
               &queued_pause.probe_subscription) == ESP_OK);
    _assert_event_slot_headroom(3);

    assert(app_manager_ui_call(_publish_scan_and_pause_on_ui, &queued_pause,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(queued_pause.probe_subscription ==
           EVENT_BUS_SUB_HANDLE_INVALID);
    assert(host_wifi_service_current_session() == 0);
    assert(host_wifi_service_current_operation() == 0);
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(atomic_load(&s_queued_scan_probe_count) == 0U);
    assert(resources.objects == 0);
    assert(resources.screens == 1U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(APP_MANAGER_ID_SETUP));
    assert(!app_page_is_actived(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_get_running_apps() == 2U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    const wifi_service_session_id_t new_session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t new_operation =
        host_wifi_service_current_operation();
    assert(new_session != 0 && new_session != old_session);
    assert(new_operation != 0 && new_operation != old_operation);
    assert(app_manager_is_actived(APP_MANAGER_ID_SETUP));
    assert(app_page_is_actived(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_get_running_apps() == 2U);
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.screens == 2U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(2);

    assert(_lifecycle_observed(
               APP_MANAGER_ID_SETUP, "root", APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_SETUP, "root", APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);
    assert(!_ui_has_text("Paused Session AP"));
    assert(_ui_has_text("Scanning..."));

    wifi_service_scan_snapshot_t current = queued_pause.snapshot;
    current.generation++;
    current.session_id = new_session;
    current.operation_id = new_operation;
    memcpy(current.records[0].ssid, "Resumed Session AP",
           sizeof("Resumed Session AP"));
    assert(host_wifi_service_publish_scan(&current) == ESP_OK);
    assert(_wait_for_text("Resumed Session AP"));

    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    _assert_event_slot_headroom(1);
}

static void _test_system_edge_back_gesture(void)
{
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    assert(app_manager_back_gesture_is_enabled());
    system_gesture_snapshot_t system = _system_gesture_snapshot();
    assert(system.object_count == 5U);
    assert(system.left_edge_found && system.right_edge_found);
    assert(system.indicator_found && system.arrow_found);
    assert(system.indicator.canvas);
    assert(system.indicator.canvas_color_format == LV_COLOR_FORMAT_A8);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.height == GESTURE_CANVAS_HEIGHT);
    assert(system.indicator.image_recolor == lv_color_hex(0x202124U));
    assert(system.indicator.image_opacity == 220);
    assert(system.indicator.canvas_flush_count == 1U);
    assert(system.indicator.invalidation_count == 1U);
    assert(system.active_screen_found);
    assert(system.visible_edge_count == 0U);
    _assert_indicator_hidden();

    /* Home remains non-interactive even when an App history target exists. */
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    system = _system_gesture_snapshot();
    assert(system.visible_edge_count == 0U);
    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 80, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 80, 220));
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    system = _system_gesture_snapshot();
    assert(system.visible_edge_count == 2U);

    const host_lv_system_object_snapshot_t menu_screen = system.active_screen;
    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    system = _system_gesture_snapshot();
    assert(system.pointer_target_found);
    assert(system.pointer_target.object == system.left_edge.object);
    assert(system.indicator_visible);
    assert(system.indicator.x == -GESTURE_CANVAS_WIDTH);
    assert(system.indicator.y == 148);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.height == GESTURE_CANVAS_HEIGHT);
    assert(system.indicator.image_opacity == 220);
    assert(system.indicator.canvas_flush_count == 1U);
    assert(system.indicator.invalidation_count == 1U);
    const gesture_curve_snapshot_t left_curve =
        _gesture_curve_snapshot(system.indicator.object);
    _assert_left_gesture_curve(&left_curve);
    _assert_same_screen(&menu_screen, &system.active_screen);
    assert(_touch(TOUCH_ACTION_MOVE, 28, 230));
    system = _system_gesture_snapshot();
    assert(system.indicator.x == -31);
    assert(system.indicator.y == 148);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.image_opacity == 220);
    _assert_same_screen(&menu_screen, &system.active_screen);
    assert(_touch(TOUCH_ACTION_MOVE, GESTURE_TRIGGER_DISTANCE, 280));
    system = _system_gesture_snapshot();
    assert(system.indicator.x == 0 && system.indicator.y == 148);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.image_opacity == 220);
    assert(system.arrow_visible);
    assert(_touch(TOUCH_ACTION_MOVE, 44, 180));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_system_gesture_snapshot().indicator.y == 148);
    assert(_touch(TOUCH_ACTION_MOVE, 43, 140));
    system = _system_gesture_snapshot();
    assert(!system.arrow_visible && system.indicator.y == 148);
    assert(_touch(TOUCH_ACTION_RELEASE, 43, 140));
    system = _system_gesture_snapshot();
    assert(!system.pointer_target_found);
    _assert_indicator_hidden();
    _assert_same_screen(&menu_screen, &system.active_screen);

    assert(_touch(TOUCH_ACTION_PRESS, 0, 0));
    system = _system_gesture_snapshot();
    assert(system.indicator_visible && system.indicator.y == 8);
    assert(_touch(TOUCH_ACTION_MOVE, 33, 10));
    assert(_system_gesture_snapshot().indicator.y == 8);
    assert(_touch(TOUCH_ACTION_RELEASE, 33, 10));
    _assert_indicator_hidden();
    assert(_touch(TOUCH_ACTION_PRESS, 0, 447));
    system = _system_gesture_snapshot();
    assert(system.indicator_visible && system.indicator.y == 296);
    assert(_touch(TOUCH_ACTION_MOVE, 33, 437));
    assert(_system_gesture_snapshot().indicator.y == 296);
    assert(_touch(TOUCH_ACTION_RELEASE, 33, 437));
    _assert_indicator_hidden();

    /* The 29 px left strip includes x=28 and excludes x=29. */
    assert(_touch(TOUCH_ACTION_PRESS, 29, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 84, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 84, 220));
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    assert(_touch(TOUCH_ACTION_PRESS, 28, 220));
    system = _system_gesture_snapshot();
    assert(system.pointer_target_found);
    assert(system.pointer_target.object == system.left_edge.object);
    assert(_touch(TOUCH_ACTION_MOVE, 83, 220));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_MOVE, 72, 220));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_MOVE, 71, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 71, 220));
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    /* A 2:1 diagonal locks horizontally but 54 px remains sub-threshold. */
    size_t lifecycle_before = s_lifecycle_observation_count;
    lv_resource_counts_t resources_before = _lv_resource_counts();
    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, GESTURE_DIRECTION_SLOP, 242));
    assert(_system_gesture_snapshot().indicator_visible);
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_MOVE, 54, 328));
    assert(_system_gesture_snapshot().indicator_visible);
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(s_lifecycle_observation_count == lifecycle_before);
    assert(!app_manager_is_transitioning());
    assert(_lv_resource_counts().screens == resources_before.screens);
    assert(_touch(TOUCH_ACTION_RELEASE, 54, 328));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 5, 243));
    _assert_indicator_hidden();
    assert(_touch(TOUCH_ACTION_RELEASE, 5, 243));
    _assert_indicator_hidden();
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 40, 220));
    assert(_system_gesture_snapshot().indicator_visible);
    assert(_touch(TOUCH_ACTION_RESET, 0, 0));
    system = _system_gesture_snapshot();
    assert(!system.pointer_target_found);
    _assert_indicator_hidden();
    assert(!_touch(TOUCH_ACTION_RELEASE, 40, 220));
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    lifecycle_before = s_lifecycle_observation_count;
    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, GESTURE_TRIGGER_DISTANCE, 220));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(s_lifecycle_observation_count == lifecycle_before);
    assert(!app_manager_is_transitioning());
    assert(_touch(TOUCH_ACTION_RELEASE, GESTURE_TRIGGER_DISTANCE, 220));
    assert(_wait_for_active(APP_MANAGER_ID_HOME));

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_navigate(APP_MANAGER_NAV_OP_OPEN_PAGE,
                     APP_MANAGER_ID_SETTINGS, "power") == ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "power"));
    lifecycle_before = s_lifecycle_observation_count;
    resources_before = _lv_resource_counts();
    assert(_touch(TOUCH_ACTION_PRESS, 0, 120));
    assert(_touch(TOUCH_ACTION_MOVE, GESTURE_TRIGGER_DISTANCE, 120));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(s_lifecycle_observation_count == lifecycle_before);
    assert(_lv_resource_counts().screens == resources_before.screens);
    assert(_touch(TOUCH_ACTION_RELEASE, GESTURE_TRIGGER_DISTANCE, 120));
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "power"));

    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 40, 220));
    assert(_system_gesture_snapshot().indicator_visible);
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    system = _system_gesture_snapshot();
    assert(!system.pointer_target_found);
    _assert_indicator_hidden();
    assert(!_touch(TOUCH_ACTION_RELEASE, 40, 220));
    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(_system_gesture_snapshot().visible_edge_count == 2U);

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_back_gesture_set_enabled(false) == ESP_OK);
    assert(!app_manager_back_gesture_is_enabled());
    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(_system_gesture_snapshot().visible_edge_count == 0U);
    assert(app_manager_back_gesture_set_enabled(true) == ESP_OK);
    assert(_system_gesture_snapshot().visible_edge_count == 2U);

    assert(app_manager_back_gesture_set_enabled(false) == ESP_OK);
    assert(!app_manager_back_gesture_is_enabled());
    assert(_system_gesture_snapshot().visible_edge_count == 0U);
    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 80, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 80, 220));
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));

    assert(app_manager_back_gesture_set_enabled(true) == ESP_OK);
    assert(app_manager_back_gesture_is_enabled());
    assert(_system_gesture_snapshot().visible_edge_count == 2U);
    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, GESTURE_TRIGGER_DISTANCE, 220));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(app_manager_back_gesture_set_enabled(false) == ESP_OK);
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_system_gesture_snapshot().visible_edge_count == 0U);
    assert(!_touch(TOUCH_ACTION_RELEASE, GESTURE_TRIGGER_DISTANCE, 220));
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(app_manager_back_gesture_set_enabled(true) == ESP_OK);
    assert(_system_gesture_snapshot().visible_edge_count == 2U);

    assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 40, 220));
    assert(_system_gesture_snapshot().indicator_visible);
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    system = _system_gesture_snapshot();
    assert(!system.pointer_target_found);
    _assert_indicator_hidden();
    assert(!_touch(TOUCH_ACTION_RELEASE, 40, 220));
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    system = _system_gesture_snapshot();
    assert(system.visible_edge_count == 2U);

    /* The 29 px right strip includes x=339 and excludes x=338. */
    assert(_touch(TOUCH_ACTION_PRESS, 338, 220));
    assert(_touch(TOUCH_ACTION_MOVE, 283, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 283, 220));
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));

    assert(_touch(TOUCH_ACTION_PRESS, 339, 220));
    system = _system_gesture_snapshot();
    assert(system.pointer_target_found);
    assert(system.pointer_target.object == system.right_edge.object);
    assert(_touch(TOUCH_ACTION_MOVE, 284, 220));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_MOVE, 296, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 296, 220));
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));

    const host_lv_system_object_snapshot_t settings_screen =
        system.active_screen;
    assert(_touch(TOUCH_ACTION_PRESS, 367, 220));
    system = _system_gesture_snapshot();
    assert(system.pointer_target_found);
    assert(system.pointer_target.object == system.right_edge.object);
    assert(system.indicator.x == 368);
    assert(system.indicator.y == 148);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.height == GESTURE_CANVAS_HEIGHT);
    assert(system.indicator.image_opacity == 220);
    assert(system.indicator.canvas_flush_count == 2U);
    assert(system.indicator.invalidation_count == 2U);
    const gesture_curve_snapshot_t right_curve =
        _gesture_curve_snapshot(system.indicator.object);
    _assert_mirrored_gesture_curve(&left_curve, &right_curve);
    assert(_touch(TOUCH_ACTION_MOVE, 339, 230));
    system = _system_gesture_snapshot();
    assert(system.indicator.x == 335 && system.indicator.y == 148);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.image_opacity == 220);
    _assert_same_screen(&settings_screen, &system.active_screen);
    assert(_touch(TOUCH_ACTION_MOVE, 312, 280));
    system = _system_gesture_snapshot();
    assert(system.arrow_visible);
    assert(system.indicator.x == 304 && system.indicator.y == 148);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.image_opacity == 220);
    _assert_same_screen(&settings_screen, &system.active_screen);
    assert(_touch(TOUCH_ACTION_RELEASE, 312, 280));
    assert(_wait_for_active(APP_MANAGER_ID_HOME));

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    const wifi_service_session_id_t session =
        host_wifi_service_current_session();
    const wifi_service_operation_id_t operation =
        host_wifi_service_current_operation();
    assert(session != 0 && operation != 0);
    assert(_touch(TOUCH_ACTION_PRESS, 0, 300));
    assert(_touch(TOUCH_ACTION_MOVE, 40, 300));
    assert(_touch(TOUCH_ACTION_RELEASE, 40, 300));
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(host_wifi_service_current_session() == session);
    assert(host_wifi_service_current_operation() == operation);

    assert(_touch(TOUCH_ACTION_PRESS, 0, 300));
    assert(_touch(TOUCH_ACTION_MOVE, 56, 300));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 56, 300));
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(host_wifi_service_current_session() == 0);
    assert(host_wifi_service_current_operation() == 0);
    assert(_system_gesture_snapshot().visible_edge_count == 0U);
}

int main(void)
{
    _initialize_stack();
    _test_real_app_navigation();
    _test_home_resume_before_first_draw();
    _test_latest_power_backpressure();
    _test_latest_wifi_backpressure_and_reopen();
    _assert_real_page_start_contract();
    _test_other_real_app_screen_lifecycles();
    _test_screen_pause_finishes_transition();
    _test_home_screen_lifecycle();
    _test_setup_screen_lifecycle();
    _test_system_edge_back_gesture();

    assert(app_manager_back_gesture_shutdown_begin() == ESP_OK);
    assert(app_manager_navigation_deinit() == ESP_OK);
    assert(app_manager_lifecycle_shutdown_begin_and_wait() == ESP_OK);
    assert(app_manager_ui_call(_runtime_deinit_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(!app_manager_back_gesture_is_enabled());
    assert(_lv_resource_counts().screens == 0U);
    assert(_system_gesture_snapshot().object_count == 0U);
    assert(app_manager_builtin_registry_reset() == ESP_OK);

    host_task_shutdown();
    assert(app_manager_mailbox_deinit() == ESP_OK);
    app_theme_deinit();
    puts("production cross-layer integration tests passed");
    return 0;
}

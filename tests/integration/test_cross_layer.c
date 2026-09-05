#include "apps_integration_runtime.h"

#include "app_manager.h"
#include "app_manager_back_gesture.h"
#include "app_manager_builtin.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"
#include "app_manager_task_switcher.h"
#include "app_theme.h"
#include "app_theme_colors.h"
#include "event_bus.h"
#include "host_freertos.h"
#include "host_connectivity_manager.h"
#include "host_device_link_service.h"
#include "host_factory_reset_service.h"
#include "host_power.h"
#include "power_service.h"
#include "weather_app_internal.h"
#include "weather_service.h"
#include "app_image_ids.h"
#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define UI_TIMEOUT_MS 1000U
#define WAIT_ATTEMPTS 2000U
#define BUILTIN_APP_COUNT 9U
#define LIFECYCLE_OBSERVATION_CAPACITY 1536U
#define LIFECYCLE_ID_BYTES 32U

EVENT_BUS_DEFINE_ID(CROSS_LAYER_TEST_MSG);

extern const app_manager_app_desc_t _app_manager_apps_start[];
extern const app_manager_app_desc_t _app_manager_apps_end[];

static atomic_uint s_noop_count;
static atomic_uint s_fifo_completion_count;
static atomic_uint s_fifo_completion_order[2];
static atomic_int s_fifo_completion_results[2];

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

typedef struct snapshot_transition_counts
{
    size_t captures;
    size_t animation_starts;
    size_t live_animations;
} snapshot_transition_counts_t;

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

static void _fifo_navigation_completed(esp_err_t result, void *context)
{
    const unsigned index = atomic_load_explicit(&s_fifo_completion_count,
                           memory_order_relaxed);

    assert(index < sizeof(s_fifo_completion_order) /
           sizeof(s_fifo_completion_order[0]));
    atomic_store_explicit(&s_fifo_completion_order[index],
                          (unsigned)(uintptr_t)context, memory_order_relaxed);
    atomic_store_explicit(&s_fifo_completion_results[index], result,
                          memory_order_relaxed);
    atomic_store_explicit(&s_fifo_completion_count, index + 1U,
                          memory_order_release);
}

static esp_err_t _ui_barrier(void *arg)
{
    (void)arg;
    return ESP_OK;
}

static esp_err_t _step_lv_timers_on_ui(void *arg)
{
    (void)arg;
    host_lv_timer_step();
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

static esp_err_t _navigate_weather_alert_detail(uint64_t alert_key,
        uint16_t argument_type)
{
    const weather_alert_arguments_t arguments =
    {
        .alert_key = alert_key,
    };
    app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = APP_MANAGER_ID_WEATHER,
        .page_id = WEATHER_PAGE_DETAIL,
        .has_arguments = true,
        .arguments =
        {
            .version = APP_MANAGER_TYPED_BLOB_VERSION,
            .type = argument_type,
            .size = sizeof(arguments),
        },
    };
    memcpy(request.arguments.payload, &arguments, sizeof(arguments));
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

static esp_err_t _click_action_on_ui(void *arg)
{
    return host_lv_click_action(arg) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _click_back_on_ui(void *arg)
{
    (void)arg;
    return host_lv_click_back() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _toggle_switch_on_ui(void *arg)
{
    const bool checked = *(const bool *)arg;
    return host_lv_toggle_visible_switch(checked) ? ESP_OK :
           ESP_ERR_NOT_FOUND;
}

typedef struct text_query
{
    const char *text;
    bool found;
} text_query_t;

typedef struct text_count_query
{
    const char *text;
    size_t count;
} text_count_query_t;

typedef struct font_query
{
    const char *text;
    const lv_font_t *font;
    bool found;
} font_query_t;

static lv_font_t s_theme_fonts[APP_THEME_FONT_MAX];

typedef struct visible_slider_snapshot
{
    int32_t value;
    bool pressed;
    bool disabled;
} visible_slider_snapshot_t;

typedef struct label_layout_query
{
    const char *text;
    const lv_font_t *font;
    bool found;
    int long_mode;
    int32_t width;
    int32_t height;
    int32_t parent_height;
    int32_t grandparent_height;
} label_layout_query_t;

typedef struct label_input_query
{
    const char *text;
    const lv_font_t *font;
    bool found;
    bool parent_clickable;
    bool parent_scrollable;
    bool grandparent_clickable;
} label_input_query_t;

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

static esp_err_t _query_text_count_on_ui(void *arg)
{
    text_count_query_t *query = arg;
    query->count = host_lv_visible_text_count(query->text);
    return ESP_OK;
}

static esp_err_t _query_font_on_ui(void *arg)
{
    font_query_t *query = arg;
    query->found = host_lv_text_has_font(query->text, query->font);
    return ESP_OK;
}

static esp_err_t _query_label_layout_on_ui(void *arg)
{
    label_layout_query_t *query = arg;
    host_lv_system_object_snapshot_t snapshot;
    query->found = host_lv_visible_label_snapshot(query->text, query->font,
                   &snapshot);
    if (!query->found)
    {
        return ESP_OK;
    }
    query->long_mode = snapshot.label_long_mode;
    query->width = snapshot.width;
    query->height = snapshot.height;
    lv_obj_t *parent = lv_obj_get_parent(snapshot.object);
    lv_obj_t *grandparent = parent == NULL ? NULL : lv_obj_get_parent(parent);
    query->parent_height = lv_obj_get_height(parent);
    query->grandparent_height = lv_obj_get_height(grandparent);
    return ESP_OK;
}

static esp_err_t _query_label_input_on_ui(void *arg)
{
    label_input_query_t *query = arg;
    host_lv_system_object_snapshot_t snapshot;
    query->found = host_lv_visible_label_snapshot(query->text, query->font,
                   &snapshot);
    if (!query->found)
    {
        return ESP_OK;
    }
    lv_obj_t *parent = lv_obj_get_parent(snapshot.object);
    lv_obj_t *grandparent = parent == NULL ? NULL : lv_obj_get_parent(parent);
    query->parent_clickable = lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    query->parent_scrollable = lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    query->grandparent_clickable = lv_obj_has_flag(
                                       grandparent, LV_OBJ_FLAG_CLICKABLE);
    return ESP_OK;
}

static esp_err_t _drag_visible_slider_on_ui(void *arg)
{
    const int32_t value = *(const int32_t *)arg;
    return host_lv_visible_slider_drag(value) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _release_visible_slider_on_ui(void *arg)
{
    (void)arg;
    return host_lv_visible_slider_release() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _cancel_visible_slider_on_ui(void *arg)
{
    (void)arg;
    return host_lv_visible_slider_cancel() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t _query_visible_slider_on_ui(void *arg)
{
    visible_slider_snapshot_t *snapshot = arg;
    return host_lv_visible_slider_snapshot(&snapshot->value,
                                           &snapshot->pressed,
                                           &snapshot->disabled) ? ESP_OK : ESP_ERR_NOT_FOUND;
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

static esp_err_t _query_snapshot_transition_counts_on_ui(void *arg)
{
    snapshot_transition_counts_t *counts = arg;
    counts->captures = host_lv_snapshot_capture_count();
    counts->animation_starts = host_lv_generic_animation_start_count();
    counts->live_animations = host_lv_generic_animation_count();
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

static esp_err_t _switcher_init_on_ui(void *arg)
{
    (void)arg;
    return app_manager_task_switcher_init(lv_display_get_default());
}

static esp_err_t _runtime_deinit_on_ui(void *arg)
{
    (void)arg;
    esp_err_t result = app_manager_task_switcher_deinit();
    if (result == ESP_OK)
    {
        result = app_manager_lifecycle_deinit();
    }
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

static size_t _ui_visible_text_count(const char *text)
{
    text_count_query_t query =
    {
        .text = text,
    };
    assert(app_manager_ui_call(_query_text_count_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    return query.count;
}

static bool _ui_text_has_font(const char *text,
                              const lv_font_t *font)
{
    font_query_t query =
    {
        .text = text,
        .font = font,
    };
    assert(app_manager_ui_call(_query_font_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    return query.found;
}

static label_layout_query_t _ui_label_layout(const char *text,
        const lv_font_t *font)
{
    label_layout_query_t query =
    {
        .text = text,
        .font = font,
    };
    assert(app_manager_ui_call(_query_label_layout_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(query.found);
    return query;
}

static label_input_query_t _ui_label_input(const char *text,
        const lv_font_t *font)
{
    label_input_query_t query =
    {
        .text = text,
        .font = font,
    };
    assert(app_manager_ui_call(_query_label_input_on_ui, &query,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(query.found);
    return query;
}

static snapshot_transition_counts_t _snapshot_transition_counts(void)
{
    snapshot_transition_counts_t counts = {0};
    assert(app_manager_ui_call(_query_snapshot_transition_counts_on_ui,
                               &counts, UI_TIMEOUT_MS) == ESP_OK);
    return counts;
}

static void _drag_visible_slider(int32_t value)
{
    assert(app_manager_ui_call(_drag_visible_slider_on_ui, &value,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static void _release_visible_slider(void)
{
    assert(app_manager_ui_call(_release_visible_slider_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static void _cancel_visible_slider(void)
{
    assert(app_manager_ui_call(_cancel_visible_slider_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
}

static visible_slider_snapshot_t _visible_slider_snapshot(void)
{
    visible_slider_snapshot_t snapshot = {0};
    assert(app_manager_ui_call(_query_visible_slider_on_ui, &snapshot,
                               UI_TIMEOUT_MS) == ESP_OK);
    return snapshot;
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
    const esp_err_t result = app_manager_ui_call(
                                 _click_action_on_ui, (void *)title,
                                 UI_TIMEOUT_MS);
    if (result != ESP_OK)
    {
        fprintf(stderr, "click_action failed: %s\\n", title);
    }
    assert(result == ESP_OK);
}

static void _click_back(void)
{
    const esp_err_t result = app_manager_ui_call(_click_back_on_ui, NULL,
                             UI_TIMEOUT_MS);
    if (result == ESP_ERR_NOT_FOUND)
    {
        /* Pages without a title button use the same left-edge gesture as the
         * device.  Keep the test on the real input path instead of invoking
         * navigation directly. */
        assert(_touch(TOUCH_ACTION_PRESS, 0, 220));
        assert(_touch(TOUCH_ACTION_MOVE, GESTURE_TRIGGER_DISTANCE, 220));
        assert(_touch(TOUCH_ACTION_RELEASE, GESTURE_TRIGGER_DISTANCE, 220));
    }
    else
    {
        assert(result == ESP_OK);
    }
}

static void _toggle_switch(bool checked)
{
    assert(app_manager_ui_call(_toggle_switch_on_ui, &checked,
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

static bool _wait_for_text_with_timers(const char *text)
{
    bool found = false;
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS && !found; ++attempt)
    {
        assert(app_manager_ui_call(_step_lv_timers_on_ui, NULL,
                                   UI_TIMEOUT_MS) == ESP_OK);
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

static __attribute__((unused)) bool _wait_for_time_alarm_state(bool expected)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_time_alarm_is_enabled() == expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static __attribute__((unused)) bool _wait_for_time_sync_state(bool expected)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_time_sync_is_owned() == expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static __attribute__((unused)) bool _wait_for_time_sync_request_entered(void)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_time_sync_request_entered())
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_for_dynamic_task_count(size_t expected)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_dynamic_task_count() == expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_for_audio_read_count(unsigned minimum)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_audio_read_count() >= minimum)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_for_audio_volume(uint8_t expected)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_audio_volume() == expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_for_audio_set_volume_count(unsigned expected)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (host_audio_set_volume_count() >= expected)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_for_visible_slider_enabled(void)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        assert(app_manager_ui_call(_step_lv_timers_on_ui, NULL,
                                   UI_TIMEOUT_MS) == ESP_OK);
        const visible_slider_snapshot_t slider = _visible_slider_snapshot();
        if (!slider.disabled)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
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

static __attribute__((unused)) void _exercise_audio_volume_slider(void)
{
    assert(_wait_for_text_with_timers("60%"));
    const unsigned reads_before = host_audio_read_count();
    const unsigned sets_before = host_audio_set_volume_count();

    _drag_visible_slider(67);
    assert(_ui_has_text("67%"));
    _cancel_visible_slider();
    visible_slider_snapshot_t slider = _visible_slider_snapshot();
    assert(slider.value == 60);
    assert(!slider.pressed);
    assert(!slider.disabled);
    assert(_ui_has_text("60%"));
    assert(host_audio_set_volume_count() == sets_before);

    _drag_visible_slider(73);
    slider = _visible_slider_snapshot();
    assert(slider.value == 73);
    assert(slider.pressed);
    assert(!slider.disabled);
    assert(_ui_has_text("73%"));
    assert(host_audio_volume() == 60U);
    assert(host_audio_set_volume_count() == sets_before);

    assert(_wait_for_audio_read_count(reads_before + 2U));
    assert(app_manager_ui_call(_step_lv_timers_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    slider = _visible_slider_snapshot();
    assert(slider.value == 73);
    assert(slider.pressed);
    assert(_ui_has_text("73%"));
    assert(host_audio_volume() == 60U);
    assert(host_audio_set_volume_count() == sets_before);

    _drag_visible_slider(81);
    assert(_ui_has_text("81%"));
    assert(host_audio_volume() == 60U);
    assert(host_audio_set_volume_count() == sets_before);

    _release_visible_slider();
    assert(_wait_for_audio_volume(81U));
    assert(host_audio_set_volume_count() == sets_before + 1U);
    assert(_wait_for_visible_slider_enabled());
    slider = _visible_slider_snapshot();
    assert(slider.value == 81);
    assert(!slider.pressed);
    assert(!slider.disabled);
    assert(_ui_has_text("81%"));
    assert(host_audio_set_volume_count() == sets_before + 1U);

    host_audio_fail_next_volume();
    _drag_visible_slider(92);
    _release_visible_slider();
    assert(_wait_for_audio_set_volume_count(sets_before + 2U));
    assert(_wait_for_visible_slider_enabled());
    slider = _visible_slider_snapshot();
    assert(slider.value == 81);
    assert(!slider.pressed);
    assert(!slider.disabled);
    assert(_ui_has_text("81%"));
    assert(host_audio_volume() == 81U);
}

static void _initialize_stack(void)
{
    host_runtime_reset();
    assert(!app_manager_back_gesture_is_enabled());
    assert(app_theme_init() == ESP_OK);
    for (int id = 0; id < APP_THEME_FONT_MAX; ++id)
    {
        s_theme_fonts[id].marker = (uint8_t)(id + 10);
        assert(app_theme_set_font((app_theme_font_id_t)id,
                                  &s_theme_fonts[id]) == ESP_OK);
    }
    assert(app_manager_mailbox_init() == ESP_OK);
    assert(event_bus_init() == ESP_OK);

    app_manager_ui_dispatch_fn dispatcher = NULL;
    assert(app_manager_get_ui_dispatch_fn(&dispatcher) == ESP_OK);
    assert(dispatcher != NULL);
    assert(event_bus_register_ui_dispatch(dispatcher) == ESP_OK);

    const ptrdiff_t app_descriptor_count =
        _app_manager_apps_end - _app_manager_apps_start;
    assert(app_descriptor_count == (ptrdiff_t)BUILTIN_APP_COUNT);
    assert(app_manager_register_builtin_descriptors(
               _app_manager_apps_start,
               (size_t)app_descriptor_count) == ESP_OK);
    assert(app_manager_builtin_registry_validate() == ESP_OK);
    assert(app_manager_builtin_discover() == (int)BUILTIN_APP_COUNT);
    assert(app_manager_ui_call(_presentation_init_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_lifecycle_configure(
               0, APP_MANAGER_RESIDENT_REJECT,
               NULL, NULL, NULL, NULL) == ESP_OK);
    assert(app_manager_lifecycle_init() == ESP_OK);
    assert(app_manager_navigation_init() == ESP_OK);
    assert(app_manager_ui_call(_back_gesture_init_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_ui_call(_switcher_init_on_ui, NULL,
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
    host_power_set_snapshot(&snapshot);
    assert(event_bus_publish(
               POWER_SERVICE_MSG,
               POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE,
               &snapshot, sizeof(snapshot),
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    assert(_wait_for_text("90%"));

    _click_action("Shenzhen");
    assert(_wait_for_active(APP_MANAGER_ID_WEATHER));
    assert(_ui_has_text("Shenzhen"));
    assert(_ui_has_text("31°"));
    assert(_ui_has_text("多云"));
    assert(_ui_has_text("体感36°  高34°  低27°"));
    assert(_ui_has_text("08-05 08:00 更新"));
    assert(_ui_has_text("72%"));
    assert(_ui_has_text("1.8 mm"));
    assert(_ui_visible_text_count(LV_SYMBOL_IMAGE) >= 5U);
    assert(_ui_text_has_font("Shenzhen", &s_theme_fonts[APP_THEME_FONT_HEAD]));
    assert(_ui_text_has_font("31°", &s_theme_fonts[APP_THEME_FONT_TITLE]));
    assert(_ui_text_has_font("多云", &s_theme_fonts[APP_THEME_FONT_HEAD]));
    assert(_ui_text_has_font("08-05 08:00 更新",
                             &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_ui_text_has_font("72%", &s_theme_fonts[APP_THEME_FONT_SMALL]));
    assert(_ui_text_has_font("09:00", &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_ui_text_has_font(LV_SYMBOL_REFRESH, LV_FONT_DEFAULT));
    assert(_ui_text_has_font(LV_SYMBOL_IMAGE, LV_FONT_DEFAULT));
    assert(_ui_text_has_font(LV_SYMBOL_RIGHT, LV_FONT_DEFAULT));
    _assert_event_slot_headroom(1U);

    assert(host_weather_publish(false) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    const label_layout_query_t city_layout = _ui_label_layout(
            "Shenzhen", &s_theme_fonts[APP_THEME_FONT_HEAD]);
    const label_layout_query_t hero_layout = _ui_label_layout(
            "31°", &s_theme_fonts[APP_THEME_FONT_TITLE]);
    const label_layout_query_t range_layout = _ui_label_layout(
            "体感36°  高34°  低27°", &s_theme_fonts[APP_THEME_FONT_BODY]);
    const label_layout_query_t status_layout =
        _ui_label_layout("08-05 08:00 更新",
                         &s_theme_fonts[APP_THEME_FONT_BODY]);
    const label_layout_query_t metric_layout = _ui_label_layout(
            "72%", &s_theme_fonts[APP_THEME_FONT_SMALL]);
    const label_layout_query_t hourly_layout = _ui_label_layout(
            "09:00", &s_theme_fonts[APP_THEME_FONT_BODY]);
    const label_layout_query_t detail_layout =
        _ui_label_layout("查看详细预报",
                         &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(city_layout.long_mode == LV_LABEL_LONG_SCROLL_CIRCULAR);
    assert(city_layout.height == 32);
    assert(hero_layout.parent_height == LV_SIZE_CONTENT);
    assert(hero_layout.grandparent_height == LV_SIZE_CONTENT);
    assert(range_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(range_layout.parent_height == LV_SIZE_CONTENT);
    assert(status_layout.long_mode == LV_LABEL_LONG_SCROLL_CIRCULAR);
    assert(status_layout.height == 22);
    assert(metric_layout.parent_height == 58);
    assert(metric_layout.grandparent_height == LV_SIZE_CONTENT);
    assert(hourly_layout.parent_height == 88);
    assert(hourly_layout.grandparent_height == 88);
    assert(detail_layout.parent_height == 44);

    host_weather_set_city("Shenzhen-Guangdong-Weather-Location");
    assert(host_weather_publish(false) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    const label_layout_query_t long_city_layout =
        _ui_label_layout("Shenzhen-Guangdong-Weather-Location",
                         &s_theme_fonts[APP_THEME_FONT_HEAD]);
    assert(long_city_layout.long_mode == LV_LABEL_LONG_SCROLL_CIRCULAR);
    assert(long_city_layout.height == 32);
    assert(_ui_text_has_font("Shenzhen-Guangdong-Weather-Location",
                             &s_theme_fonts[APP_THEME_FONT_HEAD]));

    host_weather_set_city("Shenzhen");
    host_weather_set_district("Nanshan");
    assert(host_weather_publish(false) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("Shenzhen·Nanshan"));
    assert(_ui_text_has_font("Shenzhen·Nanshan",
                             &s_theme_fonts[APP_THEME_FONT_HEAD]));
    host_weather_set_district(NULL);
    assert(host_weather_publish(false) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("Shenzhen"));
    assert(!_ui_has_text("Shenzhen·"));

    host_weather_set_layout_extremes(true);
    assert(host_weather_publish(false) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("雷阵雨伴有冰雹和大风"));
    assert(_ui_has_text("体感-100°  高100°  低-100°"));
    assert(_ui_has_text("6553.5 mm"));
    assert(_ui_has_text("1000 km/h"));
    assert(_ui_has_text("1000.0 km"));
    const label_layout_query_t long_condition_layout = _ui_label_layout(
            "雷阵雨伴有冰雹和大风", &s_theme_fonts[APP_THEME_FONT_HEAD]);
    const label_layout_query_t extreme_range_layout = _ui_label_layout(
            "体感-100°  高100°  低-100°",
            &s_theme_fonts[APP_THEME_FONT_BODY]);
    const label_layout_query_t extreme_metric_layout = _ui_label_layout(
            "1000 km/h", &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(long_condition_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(long_condition_layout.parent_height == LV_SIZE_CONTENT);
    assert(extreme_range_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(extreme_range_layout.parent_height == LV_SIZE_CONTENT);
    assert(extreme_metric_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(extreme_metric_layout.parent_height == 58);
    assert(extreme_metric_layout.grandparent_height == LV_SIZE_CONTENT);
    host_weather_set_layout_extremes(false);
    host_weather_set_city("Shenzhen");
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    unsigned refreshes = host_weather_refresh_count();
    _click_action(LV_SYMBOL_REFRESH);
    assert(host_weather_refresh_count() == refreshes + 1U);
    assert(_ui_has_text("已请求更新"));
    host_weather_set_refresh_result(ESP_ERR_TIMEOUT);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 23U);
    _click_action(LV_SYMBOL_REFRESH);
    assert(_ui_has_text("23 秒后可刷新"));
    host_weather_set_refresh_result(ESP_OK);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_READY, 0U);

    _click_action("查看详细预报");
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "forecast"));
    assert(_ui_has_text("实况"));
    assert(_ui_has_text("逐小时"));
    assert(_ui_has_text("7 天"));
    assert(_ui_has_text("35.6°C"));
    assert(_ui_has_text("数据来源：maxmind"));
    assert(_ui_text_has_font("实况", &s_theme_fonts[APP_THEME_FONT_SMALL]));
    assert(_ui_text_has_font("35.6°C",
                             &s_theme_fonts[APP_THEME_FONT_SMALL]));
    host_weather_set_service_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("服务状态：服务认证失败，显示已有数据"));
    host_weather_set_service_state(WEATHER_SERVICE_STATE_READY, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    _click_action("逐小时");
    assert(_ui_has_text("33°C"));
    assert(_ui_has_text("30°C"));
    assert(_ui_has_text("28°C"));
    assert(_ui_text_has_font("33°C", &s_theme_fonts[APP_THEME_FONT_BODY]));
    const label_layout_query_t chart_scale_layout =
        _ui_label_layout("33°C", &s_theme_fonts[APP_THEME_FONT_BODY]);
    assert(chart_scale_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(chart_scale_layout.width == LV_SIZE_CONTENT);
    assert(chart_scale_layout.height == LV_SIZE_CONTENT);
    assert(chart_scale_layout.parent_height == 116);
    assert(_ui_has_text("降水 0%"));
    assert(_ui_has_text("湿度 65% · 10 km/h"));
    assert(_ui_text_has_font("湿度 65% · 10 km/h",
                             &s_theme_fonts[APP_THEME_FONT_BODY]));
    const label_layout_query_t hourly_temperature_layout =
        _ui_label_layout("32°", &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(hourly_temperature_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(hourly_temperature_layout.width == LV_SIZE_CONTENT);
    assert(hourly_temperature_layout.height == LV_SIZE_CONTENT);
    assert(hourly_temperature_layout.parent_height == LV_SIZE_CONTENT);
    assert(hourly_temperature_layout.grandparent_height == LV_SIZE_CONTENT);
    const label_layout_query_t hourly_details_layout =
        _ui_label_layout("湿度 65% · 10 km/h",
                         &s_theme_fonts[APP_THEME_FONT_BODY]);
    assert(hourly_details_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(hourly_details_layout.height == LV_SIZE_CONTENT);
    assert(hourly_details_layout.parent_height == LV_SIZE_CONTENT);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("湿度 65% · 10 km/h"));

    host_weather_set_layout_extremes(true);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("-101°C"));
    assert(_ui_has_text("-100°"));
    const label_layout_query_t extreme_scale_layout = _ui_label_layout(
            "-101°C", &s_theme_fonts[APP_THEME_FONT_BODY]);
    const label_layout_query_t extreme_hour_layout = _ui_label_layout(
            "-100°", &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(extreme_scale_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(extreme_scale_layout.width == LV_SIZE_CONTENT);
    assert(extreme_hour_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(extreme_hour_layout.width == LV_SIZE_CONTENT);

    _click_action("7 天");
    assert(_ui_has_text("雷阵雨伴有冰雹 / 雷阵雨伴有冰雹"));
    assert(_ui_has_text("降水 6553.5 mm · UV 100"));
    const label_layout_query_t long_daily_layout = _ui_label_layout(
            "雷阵雨伴有冰雹 / 雷阵雨伴有冰雹",
            &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(long_daily_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(long_daily_layout.parent_height == LV_SIZE_CONTENT);
    host_weather_set_layout_extremes(false);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("晴 / 晴"));
    assert(_ui_has_text("降水 0.0 mm · UV 7"));
    assert(_ui_text_has_font("晴 / 晴",
                             &s_theme_fonts[APP_THEME_FONT_SMALL]));
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("晴 / 晴"));

    const uint32_t forecast_only = WEATHER_SERVICE_DATA_LOCATION |
                                   WEATHER_SERVICE_DATA_ALERTS |
                                   WEATHER_SERVICE_DATA_HOURLY |
                                   WEATHER_SERVICE_DATA_DAILY;
    host_weather_set_available_mask(forecast_only);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    _click_action("实况");
    assert(_ui_has_text("暂无实况数据"));
    assert(_ui_has_text("服务状态：服务认证失败，显示已有数据"));
    host_weather_set_service_state(WEATHER_SERVICE_STATE_READY, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    _click_action("逐小时");
    assert(_ui_has_text("湿度 65% · 10 km/h"));
    host_weather_set_available_mask(
        WEATHER_SERVICE_DATA_LOCATION | WEATHER_SERVICE_DATA_CURRENT |
        WEATHER_SERVICE_DATA_ALERTS | WEATHER_SERVICE_DATA_HOURLY |
        WEATHER_SERVICE_DATA_DAILY);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("湿度 65% · 10 km/h"));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "root"));

    host_weather_set_available_mask(forecast_only);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("--°"));
    assert(_ui_has_text("共 2 条气象预警"));
    assert(_ui_has_text("09:00"));
    host_weather_set_available_mask(
        WEATHER_SERVICE_DATA_LOCATION | WEATHER_SERVICE_DATA_CURRENT |
        WEATHER_SERVICE_DATA_ALERTS | WEATHER_SERVICE_DATA_HOURLY |
        WEATHER_SERVICE_DATA_DAILY);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    host_weather_set_current_freshness(false, true);
    host_weather_set_location_reused(true);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("认证失败 · 实况过期 · 沿用位置"));
    host_weather_set_current_freshness(false, false);
    host_weather_set_location_reused(false);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_READY, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    host_weather_set_current_freshness(true, false);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_DEGRADED, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("服务降级 · 缓存实况"));
    host_weather_set_current_freshness(false, true);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("服务降级 · 实况过期"));
    host_weather_set_current_freshness(false, false);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_READY, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);

    host_weather_set_alert_freshness(true, false);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    const label_layout_query_t alert_banner_layout =
        _ui_label_layout("共 2 条气象预警 · 缓存",
                         &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(alert_banner_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(alert_banner_layout.parent_height == LV_SIZE_CONTENT);
    _click_action("共 2 条气象预警 · 缓存");
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "alerts"));
    assert(_ui_has_text("共 2 条气象预警，使用缓存数据"));
    host_weather_set_alert_freshness(false, false);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("共 2 条气象预警"));
    host_weather_set_available_mask(
        WEATHER_SERVICE_DATA_LOCATION | WEATHER_SERVICE_DATA_CURRENT |
        WEATHER_SERVICE_DATA_HOURLY | WEATHER_SERVICE_DATA_DAILY);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("请求受限，显示已有数据"));
    host_weather_set_available_mask(
        WEATHER_SERVICE_DATA_LOCATION | WEATHER_SERVICE_DATA_CURRENT |
        WEATHER_SERVICE_DATA_ALERTS | WEATHER_SERVICE_DATA_HOURLY |
        WEATHER_SERVICE_DATA_DAILY);
    host_weather_set_service_state(WEATHER_SERVICE_STATE_READY, 0U);
    assert(host_weather_publish(true) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("共 2 条气象预警"));
    assert(_ui_has_text("severe · active\n08-05 08:00 - 08-05 14:00"));
    assert(_ui_text_has_font("暴雨红色预警",
                             &s_theme_fonts[APP_THEME_FONT_SMALL]));
    const label_layout_query_t alert_title_layout =
        _ui_label_layout("暴雨红色预警",
                         &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(alert_title_layout.long_mode == LV_LABEL_LONG_WRAP);
    assert(alert_title_layout.grandparent_height == LV_SIZE_CONTENT);
    const label_input_query_t alert_title_input = _ui_label_input(
            "暴雨红色预警", &s_theme_fonts[APP_THEME_FONT_SMALL]);
    assert(!alert_title_input.parent_clickable);
    assert(!alert_title_input.parent_scrollable);
    assert(alert_title_input.grandparent_clickable);
    assert(_ui_text_has_font(LV_SYMBOL_WARNING, LV_FONT_DEFAULT));
    _click_action("暴雨红色预警");
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "alert-detail"));
    assert(_ui_has_text("预计未来三小时有强降雨。"));
    assert(_ui_has_text("请减少外出。"));
    assert(_ui_text_has_font("预计未来三小时有强降雨。",
                             &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_navigate_weather_alert_detail(
               UINT64_C(0x5678), WEATHER_ARGUMENT_ALERT_KEY) == ESP_OK);
    assert(_ui_has_text("预计午后最高气温超过三十七度。"));
    assert(_ui_has_text("请减少户外活动。"));
    assert(_navigate_weather_alert_detail(
               UINT64_C(0x1234), WEATHER_ARGUMENT_ALERT_KEY + 1U) == ESP_OK);
    assert(_ui_has_text("该预警已失效或被撤销"));
    assert(host_weather_publish(false) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_has_text("该预警已失效或被撤销"));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "alerts"));
    assert(_ui_has_text("当前没有生效预警"));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "root"));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(host_weather_publish(true) == ESP_OK);

    /* Home reaches the launcher via a physical HOME key (no host key stub);
     * drive the same RUN op the power manager submits. */
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    assert(app_manager_is_page_present(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_MENU, "root"));
    /* Home must cancel both UI subscriptions as part of real ONPAUSE. */
    _assert_event_slot_headroom(0);

    static const struct
    {
        const char *action;
        const char *app_id;
        const char *visible_text;
    } product_apps[] =
    {
        {"时钟", APP_MANAGER_ID_CLOCK, "时钟"},
        {"录音", APP_MANAGER_ID_RECORDER, "录音服务不可用"},
        {"水平仪", APP_MANAGER_ID_LEVEL, "水平仪"},
    };
    for (size_t index = 0U; index < sizeof(product_apps) /
            sizeof(product_apps[0]); ++index)
    {
        _click_action(product_apps[index].action);
        assert(_wait_for_active(product_apps[index].app_id));
        assert(_ui_has_text(product_apps[index].visible_text));
        assert(_navigate(APP_MANAGER_NAV_OP_EXIT, product_apps[index].app_id,
                         NULL) == ESP_OK);
        assert(_wait_for_active(APP_MANAGER_ID_MENU));
    }
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_DIAGNOSTICS,
                     NULL) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_DIAGNOSTICS));
    assert(_ui_has_text("诊断页面仅供维护使用"));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_DIAGNOSTICS,
                     NULL) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    _click_action("系统设置");
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "root"));

    _start_first_frame_navigation(
        APP_MANAGER_NAV_OP_OPEN_PAGE, APP_MANAGER_ID_SETTINGS, "display",
        "亮度", "读取中");
    assert(_wait_for_transitioning());
    assert(_transition_target_has_text("亮度"));
    assert(!_transition_target_has_text("读取中"));
    assert(_lv_resource_counts().timers == 0U);
    _assert_event_slot_headroom(0);
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "display"));
    assert(_ui_has_text("拖动即时预览,松手保存"));
    assert(_wait_for_first_frame_completion());
    _assert_first_frame_probe(0U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "display"));

    _click_action("关于与维护");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "about"));
    assert(_ui_has_text("test-version"));
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
    assert(host_connectivity_manager_current_operation() == 0U);
    assert(_wait_for_text("手机绑定"));
    assert(!_ui_has_text("扫描网络"));
    assert(!_ui_has_text("选择网络"));
    assert(!_ui_has_text("输入密码"));

    const unsigned opens = host_device_link_service_open_count();
    const unsigned closes = host_device_link_service_close_count();
    _click_action("手机绑定");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETUP, "provisioning"));
    assert(_wait_for_text("MT"));
    assert(_wait_for_text("等待手机连接"));
    assert(host_device_link_service_open_count() >= opens + 1U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETUP, "root"));
    assert(host_device_link_service_close_count() >= closes + 1U);
    assert(!device_link_service_is_active());

    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
}

static void _test_home_resume_before_first_draw(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    /* Home reaches the launcher via a physical HOME key (no host key stub);
     * drive the same RUN op the power manager submits. */
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    _start_first_frame_navigation(
        APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, "root", "08:30", "--:--");
    assert(_wait_for_transitioning());
    assert(_transition_target_has_text("08:30"));
    assert(!_transition_target_has_text("--:--"));
    assert(_lv_resource_counts().timers == 1U);
    _assert_event_slot_headroom(3);

    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(_wait_for_first_frame_completion());
    _assert_first_frame_probe(1U);
    assert(_ui_has_text("08:30"));
    assert(_lv_resource_counts().timers == 1U);
    _assert_event_slot_headroom(3);
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

    const unsigned mailbox_available =
        CONFIG_APP_MANAGER_MAILBOX_CAPACITY - 1U;
    for (unsigned index = 0; index < mailbox_available; ++index)
    {
        assert(app_manager_ui_post(_noop_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_noop_count) == 0U);

    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS &&
            atomic_load(&s_noop_count) != mailbox_available; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load(&s_noop_count) == mailbox_available);
    app_manager_mailbox_host_timer_pause(false);
    /* The fake service serves the stored snapshot; the coalesced UI_LATEST
     * delivery must reflect the final (99%) publish. */
    host_power_set_snapshot(&snapshot);
    assert(_wait_for_text_with_timers("99%"));
}

static esp_err_t _publish_status_and_exit_setup_on_ui(void *arg)
{
    const connectivity_manager_status_snapshot_t *snapshot = arg;
    esp_err_t result = event_bus_publish(
                           CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           snapshot, sizeof(*snapshot),
                           EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    if (result != ESP_OK)
    {
        return result;
    }
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT,
        .app_id = APP_MANAGER_ID_SETUP,
    };
    return app_manager_navigate_async(&request, NULL, NULL);
}

static void _test_latest_wifi_backpressure_and_reopen(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    connectivity_manager_status_snapshot_t status =
    {
        .state = CONNECTIVITY_MANAGER_STATE_IP_READY,
        .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
        .last_error = ESP_OK,
        .ipv4_address = UINT32_C(0x0100000a),
        .available = true,
        .radio_available = true,
        .saved_profile = true,
        .profile_persisted = true,
        .auto_connect = true,
    };
    memcpy(status.ssid, "Saved WiFi", sizeof("Saved WiFi"));
    assert(host_connectivity_manager_publish_status(&status) == ESP_OK);
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(_wait_for_text("已连接"));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(_ui_text_has_font(
               "已连接", &s_theme_fonts[APP_THEME_FONT_HEAD]));
    assert(_ui_text_has_font(
               "Saved WiFi", &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_ui_text_has_font(
               "自动连接", &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_ui_text_has_font(
               "开启 2 分钟绑定窗口",
               &s_theme_fonts[APP_THEME_FONT_BODY]));

    _click_action("断开连接");
    connectivity_manager_operation_id_t operation =
        host_connectivity_manager_current_operation();
    connectivity_manager_status_snapshot_t terminal = status;
    terminal.operation_id = operation;
    terminal.operation_complete = true;
    terminal.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    terminal.ipv4_address = 0U;
    assert(host_connectivity_manager_publish_status(&terminal) == ESP_OK);
    assert(_wait_for_text("已断开连接"));

    _click_action("重新连接");
    operation = host_connectivity_manager_current_operation();
    terminal = status;
    terminal.operation_id = operation;
    terminal.operation_complete = true;
    assert(host_connectivity_manager_publish_status(&terminal) == ESP_OK);
    assert(_wait_for_text("已连接"));

    _toggle_switch(false);
    operation = host_connectivity_manager_current_operation();
    terminal.operation_id = operation;
    terminal.operation_complete = true;
    terminal.auto_connect = false;
    assert(host_connectivity_manager_publish_status(&terminal) == ESP_OK);
    assert(_wait_for_text("自动连接已关闭"));

    _toggle_switch(true);
    operation = host_connectivity_manager_current_operation();
    terminal.operation_id = operation;
    terminal.operation_complete = true;
    terminal.auto_connect = true;
    assert(host_connectivity_manager_publish_status(&terminal) == ESP_OK);
    assert(_wait_for_text("自动连接已启用"));

    _click_action("忘记网络");
    operation = host_connectivity_manager_current_operation();
    memset(&terminal, 0, sizeof(terminal));
    terminal.operation_id = operation;
    terminal.operation_complete = true;
    terminal.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    terminal.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    terminal.last_error = ESP_OK;
    terminal.available = true;
    terminal.radio_available = true;
    assert(host_connectivity_manager_publish_status(&terminal) == ESP_OK);
    assert(_wait_for_text("已忘记网络"));

    assert(host_connectivity_manager_publish_status(&status) == ESP_OK);
    assert(_wait_for_text("已连接"));

    atomic_store(&s_noop_count, 0U);
    app_manager_mailbox_host_timer_pause(true);
    for (unsigned index = 0; index < 1000U; ++index)
    {
        (void)snprintf(status.ssid, sizeof(status.ssid),
                       "WiFi %03u", index);
        assert(host_connectivity_manager_publish_status(&status) == ESP_OK);
    }

    const unsigned mailbox_available =
        CONFIG_APP_MANAGER_MAILBOX_CAPACITY - 1U;
    for (unsigned index = 0; index < mailbox_available; ++index)
    {
        assert(app_manager_ui_post(_noop_callback, NULL) == ESP_OK);
    }
    assert(app_manager_ui_post(_noop_callback, NULL) == ESP_ERR_NO_MEM);
    assert(atomic_load(&s_noop_count) == 0U);
    app_manager_mailbox_host_timer_step();
    app_manager_mailbox_host_timer_step();
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS &&
            atomic_load(&s_noop_count) != mailbox_available; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load(&s_noop_count) == mailbox_available);
    app_manager_mailbox_host_timer_pause(false);
    assert(_ui_has_text("WiFi 999"));

    const unsigned disconnects = host_connectivity_manager_call_count(
                                     HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_DISCONNECT);
    _click_action("断开连接");
    const connectivity_manager_operation_id_t old_operation =
        host_connectivity_manager_current_operation();
    assert(old_operation != 0U);
    assert(host_connectivity_manager_call_count(
               HOST_CONNECTIVITY_MANAGER_CALL_REQUEST_DISCONNECT) ==
           disconnects + 1U);
    connectivity_manager_status_snapshot_t queued =
    {
        .generation = UINT64_C(100000),
        .operation_id = old_operation,
        .state = CONNECTIVITY_MANAGER_STATE_IDLE,
        .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
        .last_error = ESP_OK,
        .available = true,
        .radio_available = true,
        .auto_connect = true,
    };
    assert(app_manager_ui_call(_publish_status_and_exit_setup_on_ui, &queued,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    _assert_event_slot_headroom(3);

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(host_connectivity_manager_current_operation() == 0U);
    connectivity_manager_status_snapshot_t stale =
    {
        .generation = UINT64_C(200000),
        .operation_id = old_operation,
        .state = CONNECTIVITY_MANAGER_STATE_IDLE,
        .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
        .last_error = ESP_OK,
        .available = true,
        .radio_available = true,
        .auto_connect = true,
        .operation_complete = true,
    };
    assert(event_bus_publish(
               CONNECTIVITY_MANAGER_MSG,
               CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               &stale, sizeof(stale), 0U) == ESP_OK);
    assert(app_manager_ui_call(_ui_barrier, NULL, UI_TIMEOUT_MS) == ESP_OK);
    assert(!_ui_has_text("操作已取消"));
    assert(_ui_has_text("WiFi 999"));
    assert(!_ui_has_text("扫描网络"));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    _assert_event_slot_headroom(3);
}

static void _test_optional_services_unavailable(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    host_optional_services_set_available(false);
    const connectivity_manager_status_snapshot_t offline =
    {
        .state = CONNECTIVITY_MANAGER_STATE_OFFLINE,
        .failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE,
        .last_error = ESP_ERR_NOT_SUPPORTED,
        .available = false,
    };
    assert(host_connectivity_manager_publish_status(&offline) == ESP_OK);
    assert(_wait_for_text_with_timers("时间不可用"));

    /* SD mount state now lives in the settings hub device summary. */
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(_wait_for_text_with_timers("电池未知 · SD 未挂载"));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));

    /* Home reaches the launcher via a physical HOME key (no host key stub);
     * drive the same RUN op the power manager submits. */
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    static const struct
    {
        const char *action;
        const char *app_id;
        const char *unavailable_text;
    } product_apps[] =
    {
        {"时钟", APP_MANAGER_ID_CLOCK, "--:--"},
        {"录音", APP_MANAGER_ID_RECORDER, "录音服务不可用"},
        {"水平仪", APP_MANAGER_ID_LEVEL, "传感器不可用"},
    };
    for (size_t index = 0U; index < sizeof(product_apps) /
            sizeof(product_apps[0]); ++index)
    {
        _click_action(product_apps[index].action);
        assert(_wait_for_active(product_apps[index].app_id));
        assert(_ui_has_text(product_apps[index].unavailable_text));
        assert(_navigate(APP_MANAGER_NAV_OP_EXIT, product_apps[index].app_id,
                         NULL) == ESP_OK);
        assert(_wait_for_active(APP_MANAGER_ID_MENU));
    }

    _click_action("网络设置");
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    assert(_wait_for_text("Wi-Fi 不可用"));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    host_optional_services_set_available(true);
    const connectivity_manager_status_snapshot_t idle =
    {
        .state = CONNECTIVITY_MANAGER_STATE_IDLE,
        .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
        .last_error = ESP_OK,
        .available = true,
        .radio_available = true,
        .auto_connect = true,
    };
    assert(host_connectivity_manager_publish_status(&idle) == ESP_OK);
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(_wait_for_text("08:30"));
    _assert_event_slot_headroom(3);
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(app_manager_get_running_apps() == 1U);

    const connectivity_manager_status_snapshot_t uninitialized =
    {
        .state = CONNECTIVITY_MANAGER_STATE_OFFLINE,
        .failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL,
        .last_error = ESP_ERR_INVALID_STATE,
        .available = false,
    };
    assert(host_connectivity_manager_cache_status(&uninitialized) == ESP_OK);
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    /* The home dashboard signals Wi-Fi via color only; the textual state
     * lives in the settings hub Wi-Fi summary. */
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    assert(_wait_for_text_with_timers("未连接"));
    assert(host_connectivity_manager_publish_status(&idle) == ESP_OK);
    assert(_wait_for_text_with_timers("未连接"));
    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
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
        {APP_MANAGER_ID_WEATHER, "root"},
        {APP_MANAGER_ID_WEATHER, "forecast"},
        {APP_MANAGER_ID_WEATHER, "alerts"},
        {APP_MANAGER_ID_WEATHER, "alert-detail"},
        {APP_MANAGER_ID_MENU, "root"},
        {APP_MANAGER_ID_CLOCK, "root"},
        {APP_MANAGER_ID_RECORDER, "root"},
        {APP_MANAGER_ID_LEVEL, "root"},
        {APP_MANAGER_ID_DIAGNOSTICS, "root"},
        {APP_MANAGER_ID_SETTINGS, "root"},
        {APP_MANAGER_ID_SETTINGS, "display"},
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
    size_t active_timers, size_t active_workers,
    size_t active_subscriptions, size_t active_weather_snapshots)
{
    assert(app_manager_is_actived(app_id));
    assert(app_page_is_actived(app_id, page_id));
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == active_timers);
    assert(_wait_for_dynamic_task_count(active_workers));
    _assert_event_slot_headroom(active_subscriptions);
    assert(host_weather_snapshot_lease_count() == active_weather_snapshots);

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
    assert(resources.timers == 0U);
    assert(_wait_for_dynamic_task_count(0U));
    _assert_event_slot_headroom(0);
    assert(host_weather_snapshot_lease_count() == 0U);
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
    assert(resources.timers == active_timers);
    assert(_wait_for_dynamic_task_count(active_workers));
    _assert_event_slot_headroom(active_subscriptions);
    assert(host_weather_snapshot_lease_count() == active_weather_snapshots);
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

static void _test_weather_page_screen_lifecycles(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(host_weather_snapshot_lease_count() == 0U);
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_WEATHER, NULL) ==
           ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "root"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_WEATHER, "root", "Shenzhen", 0U, 0U, 1U, 1U);

    assert(_navigate(APP_MANAGER_NAV_OP_OPEN_PAGE,
                     APP_MANAGER_ID_WEATHER, WEATHER_PAGE_FORECAST) == ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER,
                                 WEATHER_PAGE_FORECAST));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_WEATHER, WEATHER_PAGE_FORECAST,
        "详细预报", 0U, 0U, 1U, 1U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "root"));

    assert(_navigate(APP_MANAGER_NAV_OP_OPEN_PAGE,
                     APP_MANAGER_ID_WEATHER, WEATHER_PAGE_ALERTS) == ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER,
                                 WEATHER_PAGE_ALERTS));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_WEATHER, WEATHER_PAGE_ALERTS,
        "气象预警", 0U, 0U, 1U, 1U);

    assert(_navigate_weather_alert_detail(
               UINT64_C(0x1234), WEATHER_ARGUMENT_ALERT_KEY) == ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER,
                                 WEATHER_PAGE_DETAIL));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_WEATHER, WEATHER_PAGE_DETAIL,
        "预计未来三小时有强降雨。", 0U, 0U, 1U, 1U);

    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER,
                                 WEATHER_PAGE_ALERTS));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_WEATHER, "root"));
    _click_back();
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(host_weather_snapshot_lease_count() == 0U);
}

static void _test_other_real_app_screen_lifecycles(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_manager_get_running_apps() == 1U);

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_MENU, "root", "系统设置", 0U, 0U, 0U, 0U);

    static const struct
    {
        const char *action;
        const char *app_id;
        const char *visible_text;
    } product_apps[] =
    {
        {"时钟", APP_MANAGER_ID_CLOCK, "时钟"},
        {"录音", APP_MANAGER_ID_RECORDER, "录音"},
        {"水平仪", APP_MANAGER_ID_LEVEL, "水平仪"},
    };
    for (size_t index = 0U; index < sizeof(product_apps) /
            sizeof(product_apps[0]); ++index)
    {
        _click_action(product_apps[index].action);
        assert(_wait_for_active(product_apps[index].app_id));
        _exercise_real_page_screen_lifecycle(
            product_apps[index].app_id, "root",
            product_apps[index].visible_text, 1U, 0U, 0U, 0U);
        assert(_navigate(APP_MANAGER_NAV_OP_EXIT, product_apps[index].app_id,
                         NULL) == ESP_OK);
        assert(_wait_for_active(APP_MANAGER_ID_MENU));
    }

    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "root", "系统设置", 0U, 0U, 0U, 0U);

    _click_action("显示与电源");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "display"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "display", "亮度", 0U, 0U, 0U, 0U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));

    _click_action("设备状态");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "device"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "device", "SD 卡", 1U, 0U, 0U, 0U);

    _click_action("时间设置");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "time"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "time", "时间设置", 1U, 0U, 0U, 0U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "device"));

    _click_action("存储管理");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "storage"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "storage", "存储管理", 1U, 0U, 0U, 0U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "device"));
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));

    assert(host_factory_reset_service_request_count() == 0U);
    _click_action("关于与维护");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "about"));
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "about",
        "test-version", 0U, 0U, 0U, 0U);
    _click_action("恢复出厂设置");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "factory-reset"));
    assert(host_factory_reset_service_request_count() == 0U);
    _exercise_real_page_screen_lifecycle(
        APP_MANAGER_ID_SETTINGS, "factory-reset",
        "确认恢复出厂设置", 0U, 0U, 0U, 0U);
    _click_action("确认恢复出厂设置");
    assert(host_factory_reset_service_request_count() == 1U);
    assert(_ui_has_text("恢复请求已受理，正在重启"));
    assert(app_manager_ui_call(
               _click_action_on_ui, (void *)"确认恢复出厂设置",
               UI_TIMEOUT_MS) == ESP_ERR_NOT_FOUND);
    assert(host_factory_reset_service_request_count() == 1U);
    _click_back();
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "about"));
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
    _assert_event_slot_headroom(3);
}

static void _test_screen_pause_finishes_transition(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    const snapshot_transition_counts_t before =
        _snapshot_transition_counts();

    /* App-level transitions fade; only page transitions capture snapshots. */
    const app_manager_nav_request_t display_request =
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = APP_MANAGER_ID_SETTINGS,
        .page_id = "display",
    };
    assert(app_manager_navigate_async(&display_request, NULL, NULL) == ESP_OK);
    assert(_wait_for_transitioning());
    const snapshot_transition_counts_t started =
        _snapshot_transition_counts();
    assert(started.captures == before.captures + 2U);
    assert(started.animation_starts == before.animation_starts + 1U);
    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(!app_manager_is_transitioning());
    const snapshot_transition_counts_t paused_transition =
        _snapshot_transition_counts();
    assert(paused_transition.captures == started.captures);
    assert(paused_transition.animation_starts == started.animation_starts);
    assert(paused_transition.live_animations == 0U);

    const lv_resource_counts_t paused = _lv_resource_counts();
    assert(paused.objects == 0U);
    assert(paused.screens == 1U);
    assert(paused.timers == 0U);
    _assert_event_slot_headroom(0);
    assert(app_manager_get_running_apps() == 2U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(APP_MANAGER_ID_SETTINGS));
    assert(app_page_is_actived(APP_MANAGER_ID_SETTINGS, "display"));
    assert(_ui_has_text("亮度"));
    assert(_lv_resource_counts().screens == 2U);

    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETTINGS,
                     NULL) == ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
}

static void _test_fifo_navigation_finishes_snapshot_transition(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    const lv_resource_counts_t baseline = _lv_resource_counts();
    const snapshot_transition_counts_t snapshot_baseline =
        _snapshot_transition_counts();
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_SETTINGS));
    /* App transitions fade without snapshots; queue two page pushes so the
     * FIFO completion runs against real snapshot transitions. */
    const app_manager_nav_request_t first =
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = APP_MANAGER_ID_SETTINGS,
        .page_id = "wifi",
    };
    const app_manager_nav_request_t second =
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = APP_MANAGER_ID_SETTINGS,
        .page_id = "bluetooth",
    };

    atomic_store(&s_fifo_completion_count, 0U);
    for (size_t index = 0U;
            index < sizeof(s_fifo_completion_order) /
            sizeof(s_fifo_completion_order[0]); ++index)
    {
        atomic_store(&s_fifo_completion_order[index], 0U);
        atomic_store(&s_fifo_completion_results[index], ESP_OK);
    }
    assert(app_manager_navigate_async(
               &first, _fifo_navigation_completed,
               (void *)(uintptr_t)1U) == ESP_OK);
    assert(app_manager_navigate_async(
               &second, _fifo_navigation_completed,
               (void *)(uintptr_t)2U) == ESP_OK);

    for (unsigned attempt = 0U;
            attempt < WAIT_ATTEMPTS &&
            atomic_load_explicit(&s_fifo_completion_count,
                                 memory_order_acquire) < 2U; ++attempt)
    {
        _sleep_one_ms();
    }
    assert(atomic_load_explicit(&s_fifo_completion_count,
                                memory_order_acquire) == 2U);
    assert(atomic_load(&s_fifo_completion_order[0]) == 1U);
    assert(atomic_load(&s_fifo_completion_order[1]) == 2U);
    assert(atomic_load(&s_fifo_completion_results[0]) == ESP_OK);
    assert(atomic_load(&s_fifo_completion_results[1]) == ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "bluetooth"));
    assert(!app_manager_is_transitioning());
    const snapshot_transition_counts_t snapshot_finished =
        _snapshot_transition_counts();
    assert(snapshot_finished.captures >= snapshot_baseline.captures + 4U);
    assert(snapshot_finished.animation_starts >=
           snapshot_baseline.animation_starts + 2U);
    assert(snapshot_finished.live_animations == 0U);
    assert(host_lv_snapshot_live_allocation_count() == 2U);

    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETTINGS, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    const lv_resource_counts_t restored = _lv_resource_counts();
    assert(restored.objects == baseline.objects);
    assert(restored.screens == baseline.screens);
    assert(restored.timers == baseline.timers);
    assert(host_lv_snapshot_live_allocation_count() == 2U);
}

static void _test_home_screen_lifecycle(void)
{
    assert(app_manager_is_actived(APP_MANAGER_ID_HOME));
    assert(app_page_is_actived(APP_MANAGER_ID_HOME, "root"));
    assert(app_manager_get_running_apps() == 1U);
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.timers == 1U);
    _assert_event_slot_headroom(3);

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
    _assert_event_slot_headroom(3);
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
    assert(_wait_for_text("手机绑定"));
    _click_action("手机绑定");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETUP, "provisioning"));
    assert(_wait_for_text("等待手机连接"));
    assert(_ui_text_has_font(
               "手机绑定", &s_theme_fonts[APP_THEME_FONT_HEAD]));
    assert(_ui_text_has_font(
               "MT", &s_theme_fonts[APP_THEME_FONT_BIGL]));
    assert(_ui_text_has_font(
               "等待手机连接", &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_ui_text_has_font(
               "剩余 2:00", &s_theme_fonts[APP_THEME_FONT_SMALL]));
    const unsigned closes_before =
        host_device_link_service_close_count();
    const size_t starts_before = _lifecycle_observed(
                                     APP_MANAGER_ID_SETUP, "provisioning",
                                     APP_MANAGER_MSG_ONSTART,
                                     APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);
    const size_t resumes_before = _lifecycle_observed(
                                      APP_MANAGER_ID_SETUP, "provisioning",
                                      APP_MANAGER_MSG_ONRESUME,
                                      APP_MANAGER_LIFECYCLE_OBSERVER_AFTER);

    assert(app_manager_ui_call(_screen_pause_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    lv_resource_counts_t resources = _lv_resource_counts();
    assert(resources.objects == 0);
    assert(resources.screens == 1U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(0);
    assert(!app_manager_is_actived(APP_MANAGER_ID_SETUP));
    assert(!app_page_is_actived(APP_MANAGER_ID_SETUP, "root"));
    assert(app_manager_get_running_apps() == 2U);

    assert(app_manager_ui_call(_screen_resume_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(app_manager_is_actived(APP_MANAGER_ID_SETUP));
    assert(app_page_is_actived(APP_MANAGER_ID_SETUP, "provisioning"));
    assert(app_manager_is_page_present(APP_MANAGER_ID_SETUP,
                                       "provisioning"));
    assert(app_manager_get_running_apps() == 2U);
    resources = _lv_resource_counts();
    assert(resources.objects > 0);
    assert(resources.screens == 2U);
    assert(resources.timers == 0);
    _assert_event_slot_headroom(1);

    assert(_lifecycle_observed(
               APP_MANAGER_ID_SETUP, "provisioning", APP_MANAGER_MSG_ONSTART,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == starts_before);
    assert(_lifecycle_observed(
               APP_MANAGER_ID_SETUP, "provisioning", APP_MANAGER_MSG_ONRESUME,
               APP_MANAGER_LIFECYCLE_OBSERVER_AFTER) == resumes_before + 1U);

    device_link_service_status_t connected =
    {
        .generation = UINT64_C(300000),
        .state = DEVICE_LINK_SERVICE_STATE_CONNECTED,
        .last_error = ESP_OK,
        .window_remaining_ms = 110000U,
        .available = true,
        .active = true,
        .client_connected = true,
    };
    assert(host_device_link_service_publish_status(&connected) == ESP_OK);
    assert(_wait_for_text("手机已连接，等待配对"));
    assert(_ui_text_has_font(
               "手机已连接，等待配对",
               &s_theme_fonts[APP_THEME_FONT_BODY]));

    assert(host_device_link_service_offer_numeric_comparison(
               UINT64_C(0x1020304050607080), 123456U) == ESP_OK);
    device_link_service_status_t confirmation;

    assert(device_link_service_get_status(&confirmation) == ESP_OK);
    assert(_wait_for_text("核对手机上的数字后确认"));
    assert(_wait_for_text("123456"));
    assert(_ui_text_has_font(
               "核对手机上的数字后确认",
               &s_theme_fonts[APP_THEME_FONT_BODY]));
    assert(_ui_text_has_font(
               "123456", &s_theme_fonts[APP_THEME_FONT_BIGL]));
    assert(_ui_text_has_font(
               "确认绑定", &s_theme_fonts[APP_THEME_FONT_SMALL]));
    assert(_ui_text_has_font(
               "拒绝", &s_theme_fonts[APP_THEME_FONT_SMALL]));
    const unsigned confirmations_before =
        host_device_link_service_confirm_count();

    host_device_link_service_set_confirm_result(ESP_FAIL);
    _click_action("确认绑定");
    assert(_wait_for_text("确认提交失败，请重试"));
    assert(host_device_link_service_confirm_count() ==
           confirmations_before + 1U);
    assert(host_device_link_service_last_confirmation_token() ==
           confirmation.confirmation_token);

    host_device_link_service_set_confirm_result(ESP_OK);
    _click_action("确认绑定");
    assert(host_device_link_service_confirm_count() ==
           confirmations_before + 2U);
    assert(host_device_link_service_last_confirmation_token() ==
           confirmation.confirmation_token);

    device_link_service_status_t fault = confirmation;
    ++fault.generation;
    fault.state = DEVICE_LINK_SERVICE_STATE_ERROR;
    fault.last_error = ESP_FAIL;
    fault.window_remaining_ms = 0U;
    fault.client_connected = false;
    fault.pending_confirmation = false;
    fault.confirmation_token = 0U;
    assert(host_device_link_service_publish_status(&fault) == ESP_OK);
    assert(_wait_for_text("蓝牙关闭失败，需要重启"));

    assert(_navigate(APP_MANAGER_NAV_OP_EXIT, APP_MANAGER_ID_SETUP, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(host_device_link_service_close_count() >= closes_before + 1U);
    assert(!device_link_service_is_active());
    _assert_event_slot_headroom(3);
}

static void _test_system_edge_back_gesture(void)
{
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    assert(app_manager_back_gesture_is_enabled());
    system_gesture_snapshot_t system = _system_gesture_snapshot();
    /* Sys Layer holds the two gesture strips, the indicator, its arrow,
     * the permanent task-switcher tree (header, title, clear button/label,
     * content, empty-state label, RAM footer rows/bars/values) and the
     * persistent snapshot-transition overlay (overlay + from/to images). */
    assert(system.object_count == 21U);
    assert(system.left_edge_found && system.right_edge_found);
    assert(system.indicator_found && system.arrow_found);
    assert(system.indicator.canvas);
    assert(system.indicator.canvas_color_format == LV_COLOR_FORMAT_A8);
    assert(system.indicator.width == GESTURE_CANVAS_WIDTH);
    assert(system.indicator.height == GESTURE_CANVAS_HEIGHT);
    assert(system.indicator.image_recolor ==
           lv_color_hex(APP_THEME_COLOR_PLUME_HI));
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
    assert(!_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(!_touch(TOUCH_ACTION_MOVE, 80, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(!_touch(TOUCH_ACTION_RELEASE, 80, 220));
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
    assert(!_touch(TOUCH_ACTION_PRESS, 29, 220));
    assert(!_touch(TOUCH_ACTION_MOVE, 84, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(!_touch(TOUCH_ACTION_RELEASE, 84, 220));
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
                     APP_MANAGER_ID_SETTINGS, "display") == ESP_OK);
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "display"));
    lifecycle_before = s_lifecycle_observation_count;
    resources_before = _lv_resource_counts();
    assert(_touch(TOUCH_ACTION_PRESS, 0, 120));
    assert(_touch(TOUCH_ACTION_MOVE, GESTURE_TRIGGER_DISTANCE, 120));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(s_lifecycle_observation_count == lifecycle_before);
    assert(_lv_resource_counts().screens == resources_before.screens);
    assert(_touch(TOUCH_ACTION_RELEASE, GESTURE_TRIGGER_DISTANCE, 120));
    assert(_wait_for_page_active(APP_MANAGER_ID_SETTINGS, "root"));
    assert(!app_manager_is_page_present(APP_MANAGER_ID_SETTINGS, "display"));

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
    assert(!_touch(TOUCH_ACTION_PRESS, 0, 220));
    assert(!_touch(TOUCH_ACTION_MOVE, 80, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(!_touch(TOUCH_ACTION_RELEASE, 80, 220));
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
    assert(!_touch(TOUCH_ACTION_PRESS, 338, 220));
    assert(!_touch(TOUCH_ACTION_MOVE, 283, 220));
    assert(!_system_gesture_snapshot().arrow_visible);
    assert(!_touch(TOUCH_ACTION_RELEASE, 283, 220));
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
    assert(_wait_for_active(APP_MANAGER_ID_SETUP));
    _click_action("手机绑定");
    assert(_wait_for_page_active(APP_MANAGER_ID_SETUP, "provisioning"));
    assert(device_link_service_is_active());
    assert(_touch(TOUCH_ACTION_PRESS, 0, 300));
    assert(_touch(TOUCH_ACTION_MOVE, 40, 300));
    assert(_touch(TOUCH_ACTION_RELEASE, 40, 300));
    assert(_wait_for_page_active(APP_MANAGER_ID_SETUP, "provisioning"));
    assert(device_link_service_is_active());

    assert(_touch(TOUCH_ACTION_PRESS, 0, 300));
    assert(_touch(TOUCH_ACTION_MOVE, 56, 300));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 56, 300));
    assert(_wait_for_page_active(APP_MANAGER_ID_SETUP, "root"));
    assert(!device_link_service_is_active());

    assert(_touch(TOUCH_ACTION_PRESS, 0, 300));
    assert(_touch(TOUCH_ACTION_MOVE, 56, 300));
    assert(_system_gesture_snapshot().arrow_visible);
    assert(_touch(TOUCH_ACTION_RELEASE, 56, 300));
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(host_connectivity_manager_current_operation() == 0U);
    assert(_system_gesture_snapshot().visible_edge_count == 0U);
}

static esp_err_t _switcher_visible_on_ui(void *arg)
{
    bool *visible = arg;
    *visible = app_manager_task_switcher_is_visible();
    return ESP_OK;
}

static bool _switcher_visible(void)
{
    bool visible = false;
    assert(app_manager_ui_call(_switcher_visible_on_ui, &visible,
                               UI_TIMEOUT_MS) == ESP_OK);
    return visible;
}

static void _wait_for_switcher(bool expected)
{
    for (unsigned attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt)
    {
        if (_switcher_visible() == expected)
        {
            return;
        }
        _sleep_one_ms();
    }
    assert(_switcher_visible() == expected);
}

static void _open_task_switcher(void)
{
    assert(app_manager_navigation_submit_system_op(
               APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW, NULL, NULL) ==
           ESP_OK);
    _wait_for_switcher(true);
}

static void _wait_for_text_gone(const char *text)
{
    for (int i = 0; i < 2000 && _ui_has_text(text); i++)
    {
        usleep(1000);
    }
    assert(!_ui_has_text(text));
}

static void _test_system_task_switcher(void)
{
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_HOME, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_WEATHER, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_WEATHER));
    assert(_navigate(APP_MANAGER_NAV_OP_RUN, APP_MANAGER_ID_MENU, NULL) ==
           ESP_OK);
    assert(_wait_for_active(APP_MANAGER_ID_MENU));

    /* Opening the switcher keeps the foreground task running and lists every
     * eligible resident task, most recently used first, each on its own
     * card with a status line. */
    _open_task_switcher();
    assert(_wait_for_active(APP_MANAGER_ID_MENU));
    assert(_ui_has_text("最近任务"));
    assert(_ui_has_text("应用"));
    assert(_ui_has_text("当前"));
    assert(_ui_has_text("天气"));
    assert(_ui_has_text("点按切换"));
    assert(_ui_has_text("清除"));
    assert(_ui_has_text("内部内存"));
    {
        app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
        size_t task_count = 0U;
        assert(app_manager_get_recent_tasks(tasks,
                                            APP_MANAGER_MAX_RECENT_TASKS,
                                            &task_count) == ESP_OK);
        assert(task_count == 2U);
        assert(strcmp(tasks[0].app_id, APP_MANAGER_ID_MENU) == 0);
        assert(tasks[0].active);
        assert(tasks[0].icon_id == APP_IMAGE_MENU_ICON);
        assert(strcmp(tasks[1].app_id, APP_MANAGER_ID_WEATHER) == 0);
        assert(!tasks[1].active);
        assert(tasks[1].icon_id == APP_IMAGE_WEATHER_APP);
    }

    /* Tapping a background card restores that task and closes the switcher. */
    _click_action("天气");
    assert(_wait_for_active(APP_MANAGER_ID_WEATHER));
    _wait_for_switcher(false);

    /* The per-card close button removes that task; the switcher stays open
     * and the remaining card becomes the current one. */
    _open_task_switcher();
    assert(_ui_has_text("天气"));
    assert(_ui_has_text("当前"));
    _click_action(LV_SYMBOL_CLOSE);
    _wait_for_text_gone("应用");
    assert(_switcher_visible());
    assert(_ui_has_text("天气"));
    {
        app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
        size_t task_count = 0U;
        assert(app_manager_get_recent_tasks(tasks,
                                            APP_MANAGER_MAX_RECENT_TASKS,
                                            &task_count) == ESP_OK);
        assert(task_count == 1U);
        assert(strcmp(tasks[0].app_id, APP_MANAGER_ID_WEATHER) == 0);
    }

    /* Tapping the current card only dismisses the switcher. */
    _click_action("天气");
    _wait_for_switcher(false);
    assert(_wait_for_active(APP_MANAGER_ID_WEATHER));

    /* A repeated open while visible dismisses back to the task. */
    _open_task_switcher();
    assert(app_manager_navigation_submit_system_op(
               APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW, NULL, NULL) ==
           ESP_OK);
    _wait_for_switcher(false);
    assert(_wait_for_active(APP_MANAGER_ID_WEATHER));

    /* Clearing all tasks returns to Home and closes the switcher. */
    _open_task_switcher();
    _click_action("清除");
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
    _wait_for_switcher(false);

    /* The empty state shows and a plain tap on the backdrop still dismisses
     * (the memory footer stays visible while open). */
    _open_task_switcher();
    assert(_ui_has_text("暂无任务"));
    assert(_ui_has_text("内部内存"));
    assert(_touch(TOUCH_ACTION_PRESS, 184, 200));
    assert(_touch(TOUCH_ACTION_RELEASE, 184, 200));
    _wait_for_switcher(false);
    assert(_wait_for_active(APP_MANAGER_ID_HOME));
}

int main(void)
{
    _initialize_stack();
    _test_real_app_navigation();
    _test_weather_page_screen_lifecycles();
    _test_home_resume_before_first_draw();
    _test_latest_power_backpressure();
    _test_latest_wifi_backpressure_and_reopen();
    _test_optional_services_unavailable();
    _assert_real_page_start_contract();
    _test_other_real_app_screen_lifecycles();
    _test_screen_pause_finishes_transition();
    _test_fifo_navigation_finishes_snapshot_transition();
    _test_home_screen_lifecycle();
    _test_setup_screen_lifecycle();
    _test_system_edge_back_gesture();
    _test_system_task_switcher();

    assert(app_manager_back_gesture_shutdown_begin() == ESP_OK);
    assert(app_manager_navigation_deinit() == ESP_OK);
    assert(app_manager_lifecycle_shutdown_begin_and_wait() == ESP_OK);
    assert(app_manager_ui_call(_runtime_deinit_on_ui, NULL,
                               UI_TIMEOUT_MS) == ESP_OK);
    assert(!app_manager_back_gesture_is_enabled());
    assert(_lv_resource_counts().screens == 0U);
    assert(_system_gesture_snapshot().object_count == 0U);
    assert(_wait_for_dynamic_task_count(0U));
    app_manager_builtin_registry_reset();

    host_task_shutdown();
    assert(app_manager_mailbox_deinit() == ESP_OK);
    app_theme_deinit();
    puts("production cross-layer integration tests passed");
    return 0;
}

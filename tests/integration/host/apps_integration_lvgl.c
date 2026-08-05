#include "apps_integration_runtime.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_LV_OBJECT_CAPACITY 256U
#define HOST_LV_EVENT_CAPACITY  4U
#define HOST_LV_TIMER_CAPACITY  16U
#define HOST_LV_GENERIC_ANIM_CAPACITY 16U
#define HOST_LV_GENERIC_ANIM_COMPLETION_CAPACITY 64U
#define HOST_LV_SNAPSHOT_CAPTURE_CAPACITY 64U
#define HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY 4U
#define HOST_LV_SNAPSHOT_ALLOCATION_CALL_CAPACITY 16U
#define HOST_LV_TEXT_BYTES      160U
#define HOST_LV_HOR_RES         368
#define HOST_LV_VER_RES         448
#define HOST_LV_ANIM_STEP_MS    5

typedef enum
{
    HOST_LV_OBJECT_GENERIC = 0,
    HOST_LV_OBJECT_BUTTON,
    HOST_LV_OBJECT_LABEL,
    HOST_LV_OBJECT_SLIDER,
    HOST_LV_OBJECT_SWITCH,
    HOST_LV_OBJECT_BAR,
    HOST_LV_OBJECT_CHART,
    HOST_LV_OBJECT_CANVAS,
    HOST_LV_OBJECT_QRCODE,
    HOST_LV_OBJECT_IMAGE,
    HOST_LV_OBJECT_SCREEN,
    HOST_LV_OBJECT_LAYER,
} host_lv_object_kind_t;

struct lv_event_dsc_t
{
    bool live;
    lv_event_cb_t callback;
    lv_event_code_t code;
    void *user_data;
};

struct lv_display_t
{
    uint8_t marker;
};

struct lv_obj_t
{
    bool live;
    host_lv_object_kind_t kind;
    lv_obj_t *parent;
    char text[HOST_LV_TEXT_BYTES];
    int32_t slider_minimum;
    int32_t slider_maximum;
    int32_t slider_value;
    int32_t bar_start_value;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    lv_opa_t opacity;
    lv_opa_t background_opacity;
    lv_opa_t text_opacity;
    lv_color_t image_recolor;
    lv_opa_t image_opacity;
    lv_draw_buf_t *draw_buf;
    const void *image_source;
    uint32_t invalidation_count;
    uint32_t qrcode_data_length;
    bool qrcode_scrubbed;
    lv_obj_flag_t flags;
    lv_state_t state;
    const lv_font_t *text_font;
    int label_long_mode;
    void *user_data;
    uint64_t z_order;
    lv_event_dsc_t bindings[HOST_LV_EVENT_CAPACITY];
    size_t binding_count;
};

struct lv_chart_series_t
{
    lv_obj_t *chart;
    int32_t last_value;
};

struct lv_indev_t
{
    lv_point_t point;
    lv_indev_state_t state;
    lv_obj_t *active_object;
    bool sequence_active;
    bool wait_release;
};

struct lv_timer_t
{
    bool live;
    lv_timer_cb_t callback;
    uint32_t period;
    void *user_data;
    bool paused;
    bool ready;
};

struct lv_event_t
{
    lv_event_code_t code;
    void *user_data;
    lv_obj_t *target;
    lv_indev_t *indev;
    void *param;
};

typedef struct host_lv_transition
{
    bool pending;
    bool started;
    bool auto_delete;
    lv_obj_t *old_screen;
    lv_obj_t *new_screen;
    lv_screen_load_anim_t animation;
    lv_anim_t new_animation;
    lv_anim_t old_animation;
    bool new_animation_live;
    bool old_animation_live;
} host_lv_transition_t;

typedef struct host_lv_generic_animation
{
    bool live;
    lv_anim_t animation;
} host_lv_generic_animation_t;

typedef struct host_lv_snapshot_capture
{
    const lv_obj_t *object;
    bool input_blocked;
    bool object_active;
} host_lv_snapshot_capture_t;

typedef struct host_lv_snapshot_failures
{
    unsigned allocations;
    unsigned allocation_after;
    bool allocation_after_enabled;
    unsigned draw_buffer_initializations;
    unsigned captures;
    unsigned capture_after;
    bool capture_after_enabled;
    unsigned object_creations;
    unsigned object_creation_after;
    bool object_creation_after_enabled;
    unsigned animation_starts;
    unsigned native_animation_starts;
    unsigned screen_creations;
    unsigned screen_loads;
    unsigned screen_load_after;
    bool screen_load_after_enabled;
} host_lv_snapshot_failures_t;

typedef struct host_lv_snapshot_allocation
{
    void *pointer;
    size_t size;
} host_lv_snapshot_allocation_t;

const lv_font_t host_lv_default_font = {.marker = 1};

static lv_obj_t s_initial_screen;
static lv_obj_t s_top_layer;
static lv_obj_t s_sys_layer;
static lv_obj_t *s_active_screen;
static lv_display_t s_display = {.marker = 1U};
static lv_indev_t s_pointer_indev;
static lv_indev_t *s_event_indev;
static lv_obj_t s_objects[HOST_LV_OBJECT_CAPACITY];
static lv_timer_t s_timers[HOST_LV_TIMER_CAPACITY];
static host_lv_transition_t s_transition;
static host_lv_generic_animation_t
s_generic_animations[HOST_LV_GENERIC_ANIM_CAPACITY];
static int32_t s_generic_animation_completion_values[
    HOST_LV_GENERIC_ANIM_COMPLETION_CAPACITY];
static size_t s_generic_animation_start_count;
static size_t s_generic_animation_completion_count;
static host_lv_snapshot_capture_t
s_snapshot_captures[HOST_LV_SNAPSHOT_CAPTURE_CAPACITY];
static size_t s_snapshot_capture_count;
static host_lv_snapshot_failures_t s_snapshot_failures;
static host_lv_snapshot_allocation_t
s_snapshot_allocations[HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY];
static host_lv_snapshot_allocation_call_t
s_snapshot_allocation_calls[HOST_LV_SNAPSHOT_ALLOCATION_CALL_CAPACITY];
static size_t s_snapshot_allocation_call_count;
static uint64_t s_z_order;
static unsigned s_qrcode_update_count;
static unsigned s_qrcode_scrubbed_delete_count;
static lv_chart_series_t s_chart_series;

static void _host_lv_release_snapshot_allocations(void)
{
    for (size_t index = 0U;
            index < HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY; ++index)
    {
        free(s_snapshot_allocations[index].pointer);
        s_snapshot_allocations[index].pointer = NULL;
        s_snapshot_allocations[index].size = 0U;
    }
}

static bool _host_lv_track_snapshot_allocation(void *pointer, size_t size)
{
    for (size_t index = 0U;
            index < HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY; ++index)
    {
        if (s_snapshot_allocations[index].pointer == NULL)
        {
            s_snapshot_allocations[index].pointer = pointer;
            s_snapshot_allocations[index].size = size;
            return true;
        }
    }
    return false;
}

static void _host_lv_untrack_snapshot_allocation(void *pointer)
{
    for (size_t index = 0U;
            index < HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY; ++index)
    {
        if (s_snapshot_allocations[index].pointer == pointer)
        {
            s_snapshot_allocations[index].pointer = NULL;
            s_snapshot_allocations[index].size = 0U;
            return;
        }
    }
}

static void _host_lv_set_x_animation(void *variable, int32_t value);
static void _host_lv_set_y_animation(void *variable, int32_t value);
static void _host_lv_set_opacity_animation(void *variable, int32_t value);
static bool _host_lv_input_is_blocked(void);
static bool _host_lv_child_at(const lv_obj_t *parent, size_t child_index,
                              const lv_obj_t **child);

static lv_obj_t *_host_lv_allocate_object(host_lv_object_kind_t kind,
        lv_obj_t *parent)
{
    if (s_snapshot_failures.object_creations > 0U)
    {
        --s_snapshot_failures.object_creations;
        return NULL;
    }
    if (s_snapshot_failures.object_creation_after_enabled)
    {
        if (s_snapshot_failures.object_creation_after == 0U)
        {
            s_snapshot_failures.object_creation_after_enabled = false;
            return NULL;
        }
        --s_snapshot_failures.object_creation_after;
    }

    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        if (!s_objects[index].live)
        {
            lv_obj_t *object = &s_objects[index];
            memset(object, 0, sizeof(*object));
            object->live = true;
            object->kind = kind;
            object->parent = parent;
            object->opacity = LV_OPA_COVER;
            object->background_opacity = LV_OPA_TRANSP;
            object->text_opacity = LV_OPA_COVER;
            object->image_opacity = LV_OPA_COVER;
            object->z_order = ++s_z_order;
            if (kind == HOST_LV_OBJECT_BUTTON ||
                    kind == HOST_LV_OBJECT_SWITCH ||
                    kind == HOST_LV_OBJECT_GENERIC)
            {
                object->flags = LV_OBJ_FLAG_CLICKABLE;
            }
            object->flags |= LV_OBJ_FLAG_SCROLLABLE;
            if (kind == HOST_LV_OBJECT_SCREEN)
            {
                object->width = HOST_LV_HOR_RES;
                object->height = HOST_LV_VER_RES;
            }
            return object;
        }
    }
    return NULL;
}

static bool _host_lv_is_descendant(const lv_obj_t *object,
                                   const lv_obj_t *ancestor)
{
    for (const lv_obj_t *parent = object->parent;
            parent != NULL; parent = parent->parent)
    {
        if (parent == ancestor)
        {
            return true;
        }
    }
    return false;
}

static int32_t _host_lv_resolve_dimension(const lv_obj_t *object,
        int32_t value, bool horizontal)
{
    if (value < HOST_LV_PCT_BASE)
    {
        return value;
    }

    const int32_t percent = value - HOST_LV_PCT_BASE;
    const lv_obj_t *parent = object == NULL ? NULL : object->parent;
    const int32_t parent_size = parent == NULL ?
                                (horizontal ? HOST_LV_HOR_RES :
                                 HOST_LV_VER_RES) :
                                (horizontal ? parent->width : parent->height);
    return (int32_t)(((int64_t)parent_size * percent) / 100);
}

static int32_t _host_lv_absolute_position(const lv_obj_t *object,
        bool horizontal)
{
    int32_t position = 0;
    for (const lv_obj_t *parent = object; parent != NULL;
            parent = parent->parent)
    {
        position += horizontal ? parent->x : parent->y;
    }
    return position;
}

static bool _host_lv_contains_point(const lv_obj_t *object,
                                    const lv_point_t *point)
{
    if (object == NULL || point == NULL || object->width <= 0 ||
            object->height <= 0)
    {
        return false;
    }

    const int32_t left = _host_lv_absolute_position(object, true);
    const int32_t top = _host_lv_absolute_position(object, false);
    return point->x >= left && point->y >= top &&
           (int64_t)point->x < (int64_t)left + object->width &&
           (int64_t)point->y < (int64_t)top + object->height;
}

static bool _host_lv_has_descendant_text(const lv_obj_t *object,
        const char *text)
{
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        const lv_obj_t *candidate = &s_objects[index];
        if (candidate->live && candidate->kind == HOST_LV_OBJECT_LABEL &&
                _host_lv_is_descendant(candidate, object) &&
                strcmp(candidate->text, text) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool _host_lv_is_visible(const lv_obj_t *object)
{
    if (object == NULL || !object->live)
    {
        return false;
    }
    for (const lv_obj_t *parent = object; parent != NULL;
            parent = parent->parent)
    {
        if ((parent->flags & LV_OBJ_FLAG_HIDDEN) != 0U)
        {
            return false;
        }
    }
    if (object == s_active_screen || object == &s_top_layer ||
            object == &s_sys_layer)
    {
        return true;
    }
    return _host_lv_is_descendant(object, s_active_screen) ||
           _host_lv_is_descendant(object, &s_top_layer) ||
           _host_lv_is_descendant(object, &s_sys_layer);
}

static lv_obj_t *_host_lv_find_pointer_target_in(lv_obj_t *parent,
        const lv_point_t *point)
{
    uint64_t upper_z_order = UINT64_MAX;

    while (true)
    {
        lv_obj_t *child = NULL;
        for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
        {
            lv_obj_t *candidate = &s_objects[index];
            if (candidate->live && candidate->parent == parent &&
                    candidate->z_order < upper_z_order &&
                    _host_lv_is_visible(candidate) &&
                    _host_lv_contains_point(candidate, point) &&
                    (child == NULL ||
                     candidate->z_order > child->z_order))
            {
                child = candidate;
            }
        }
        if (child == NULL)
        {
            return NULL;
        }

        lv_obj_t *target = _host_lv_find_pointer_target_in(child, point);
        if (target != NULL)
        {
            return target;
        }
        if ((child->flags & LV_OBJ_FLAG_CLICKABLE) != 0U)
        {
            if ((child->state & LV_STATE_DISABLED) == 0U)
            {
                return child;
            }
        }
        upper_z_order = child->z_order;
    }
}

static lv_obj_t *_host_lv_find_pointer_target(const lv_point_t *point)
{
    lv_obj_t *target = _host_lv_find_pointer_target_in(&s_sys_layer, point);
    if (target == NULL)
    {
        target = _host_lv_find_pointer_target_in(&s_top_layer, point);
    }
    if (target == NULL && s_active_screen != NULL)
    {
        target = _host_lv_find_pointer_target_in(s_active_screen, point);
    }
    return target;
}

static bool _host_lv_input_is_blocked(void)
{
    if (s_transition.pending)
    {
        return true;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        const lv_obj_t *object = &s_objects[index];
        if (object->live &&
                (object->flags & LV_OBJ_FLAG_CLICKABLE) != 0U &&
                _host_lv_is_visible(object) &&
                (_host_lv_is_descendant(object, &s_top_layer) ||
                 (_host_lv_is_descendant(object, &s_sys_layer) &&
                  _host_lv_absolute_position(object, true) <= 0 &&
                  _host_lv_absolute_position(object, false) <= 0 &&
                  object->width >= HOST_LV_HOR_RES &&
                  object->height >= HOST_LV_VER_RES)))
        {
            return true;
        }
    }
    return false;
}

static lv_obj_t *_host_lv_find_visible_slider(bool enabled_only)
{
    lv_obj_t *slider = NULL;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        lv_obj_t *candidate = &s_objects[index];
        if (!candidate->live ||
                candidate->kind != HOST_LV_OBJECT_SLIDER ||
                !_host_lv_is_visible(candidate) ||
                (enabled_only &&
                 (candidate->state & LV_STATE_DISABLED) != 0U))
        {
            continue;
        }
        if (slider != NULL)
        {
            return NULL;
        }
        slider = candidate;
    }
    return slider;
}

static bool _host_lv_emit_with_input(lv_obj_t *object, lv_event_code_t code,
                                     lv_indev_t *indev, void *param)
{
    if (object == NULL || !object->live)
    {
        return false;
    }
    bool emitted = false;
    for (size_t index = 0;
            index < HOST_LV_EVENT_CAPACITY && object->live; ++index)
    {
        const lv_event_dsc_t binding = object->bindings[index];
        if (binding.live &&
                (binding.code == code || binding.code == LV_EVENT_ALL) &&
                binding.callback != NULL)
        {
            lv_event_t event =
            {
                .code = code,
                .user_data = binding.user_data,
                .target = object,
                .indev = indev,
                .param = param,
            };
            lv_indev_t *previous_indev = s_event_indev;
            s_event_indev = indev;
            binding.callback(&event);
            s_event_indev = previous_indev;
            emitted = true;
        }
    }
    return emitted;
}

static bool _host_lv_emit(lv_obj_t *object, lv_event_code_t code)
{
    return _host_lv_emit_with_input(object, code, NULL, NULL);
}

static void _host_lv_reset_pointer_target(lv_obj_t *owner)
{
    lv_obj_t *target = s_pointer_indev.active_object;
    if (target == NULL)
    {
        if (owner == NULL)
        {
            s_pointer_indev.sequence_active = false;
            s_pointer_indev.wait_release = false;
            s_pointer_indev.state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }
    if (owner != NULL && target != owner &&
            !_host_lv_is_descendant(target, owner))
    {
        return;
    }

    s_pointer_indev.active_object = NULL;
    s_pointer_indev.sequence_active = false;
    s_pointer_indev.wait_release = false;
    s_pointer_indev.state = LV_INDEV_STATE_RELEASED;
    if (target->live)
    {
        (void)_host_lv_emit_with_input(target, LV_EVENT_INDEV_RESET,
                                       &s_pointer_indev, NULL);
    }
}

static void _host_lv_normalize_screen(lv_obj_t *screen)
{
    if (screen != NULL && screen->live)
    {
        screen->x = 0;
        screen->y = 0;
        screen->opacity = LV_OPA_COVER;
    }
}

static void _host_lv_delete_object(lv_obj_t *object)
{
    _host_lv_reset_pointer_target(object);
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        if (s_objects[index].live &&
                _host_lv_is_descendant(&s_objects[index], object))
        {
            s_objects[index].live = false;
        }
    }
    object->live = false;
}

static void _host_lv_complete_transition(void)
{
    if (!s_transition.pending)
    {
        return;
    }

    lv_obj_t *old_screen = s_transition.old_screen;
    lv_obj_t *new_screen = s_transition.new_screen;
    if (!s_transition.started && new_screen != NULL && new_screen->live)
    {
        s_transition.started = true;
        s_active_screen = new_screen;
        (void)_host_lv_emit(new_screen, LV_EVENT_SCREEN_LOAD_START);
    }

    if (s_transition.new_animation_live)
    {
        s_transition.new_animation.exec_cb(
            s_transition.new_animation.var,
            s_transition.new_animation.end_value);
    }
    if (s_transition.old_animation_live)
    {
        s_transition.old_animation.exec_cb(
            s_transition.old_animation.var,
            s_transition.old_animation.end_value);
    }

    s_transition.pending = false;
    s_transition.new_animation_live = false;
    s_transition.old_animation_live = false;
    if (new_screen != NULL && new_screen->live)
    {
        s_active_screen = new_screen;
        (void)_host_lv_emit(new_screen, LV_EVENT_SCREEN_LOADED);
        _host_lv_normalize_screen(new_screen);
    }
    if (old_screen != NULL && old_screen->live)
    {
        (void)_host_lv_emit(old_screen, LV_EVENT_SCREEN_UNLOADED);
        if (s_transition.auto_delete && old_screen->live)
        {
            _host_lv_delete_object(old_screen);
        }
    }
    memset(&s_transition, 0, sizeof(s_transition));
}

static void _host_lv_load_immediate(lv_obj_t *screen, bool auto_delete)
{
    if (screen == NULL || !screen->live ||
            screen->kind != HOST_LV_OBJECT_SCREEN ||
            screen == s_active_screen)
    {
        return;
    }

    lv_obj_t *old_screen = s_active_screen;
    if (old_screen != NULL && old_screen->live)
    {
        (void)_host_lv_emit(old_screen, LV_EVENT_SCREEN_UNLOAD_START);
    }
    s_active_screen = screen;
    (void)_host_lv_emit(screen, LV_EVENT_SCREEN_LOAD_START);
    (void)_host_lv_emit(screen, LV_EVENT_SCREEN_LOADED);
    if (old_screen != NULL && old_screen->live)
    {
        (void)_host_lv_emit(old_screen, LV_EVENT_SCREEN_UNLOADED);
        if (auto_delete && old_screen != &s_initial_screen &&
                old_screen->live)
        {
            _host_lv_delete_object(old_screen);
        }
    }
    _host_lv_normalize_screen(screen);
}

static void _host_lv_configure_animation(lv_anim_t *animation,
        lv_obj_t *screen, lv_anim_exec_xcb_t callback,
        int32_t start, int32_t end, uint32_t time, uint32_t delay)
{
    *animation = (lv_anim_t)
    {
        .var = screen,
        .exec_cb = callback,
        .start_value = start,
        .current_value = start,
        .end_value = end,
        .duration = (int32_t)time,
        .act_time = -(int32_t)delay,
    };
    callback(screen, start);
}

static void _host_lv_prepare_transition_animations(uint32_t time,
        uint32_t delay)
{
    lv_obj_t *old_screen = s_transition.old_screen;
    lv_obj_t *new_screen = s_transition.new_screen;
    lv_anim_exec_xcb_t new_callback = _host_lv_set_x_animation;
    int32_t new_start = 0;
    int32_t new_end = 0;
    lv_anim_exec_xcb_t old_callback = NULL;
    int32_t old_start = 0;
    int32_t old_end = 0;

    switch (s_transition.animation)
    {
    case LV_SCREEN_LOAD_ANIM_OVER_LEFT:
        new_start = HOST_LV_HOR_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_OVER_RIGHT:
        new_start = -HOST_LV_HOR_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_OVER_TOP:
        new_callback = _host_lv_set_y_animation;
        new_start = HOST_LV_VER_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_OVER_BOTTOM:
        new_callback = _host_lv_set_y_animation;
        new_start = -HOST_LV_VER_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_MOVE_LEFT:
        new_start = HOST_LV_HOR_RES;
        old_callback = _host_lv_set_x_animation;
        old_end = -HOST_LV_HOR_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_MOVE_RIGHT:
        new_start = -HOST_LV_HOR_RES;
        old_callback = _host_lv_set_x_animation;
        old_end = HOST_LV_HOR_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_MOVE_TOP:
        new_callback = _host_lv_set_y_animation;
        new_start = HOST_LV_VER_RES;
        old_callback = _host_lv_set_y_animation;
        old_end = -HOST_LV_VER_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM:
        new_callback = _host_lv_set_y_animation;
        new_start = -HOST_LV_VER_RES;
        old_callback = _host_lv_set_y_animation;
        old_end = HOST_LV_VER_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_FADE_IN:
        new_callback = _host_lv_set_opacity_animation;
        new_start = LV_OPA_TRANSP;
        new_end = LV_OPA_COVER;
        break;
    case LV_SCREEN_LOAD_ANIM_FADE_OUT:
        old_callback = _host_lv_set_opacity_animation;
        old_start = LV_OPA_COVER;
        old_end = LV_OPA_TRANSP;
        break;
    case LV_SCREEN_LOAD_ANIM_OUT_LEFT:
        old_callback = _host_lv_set_x_animation;
        old_end = -HOST_LV_HOR_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_OUT_RIGHT:
        old_callback = _host_lv_set_x_animation;
        old_end = HOST_LV_HOR_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_OUT_TOP:
        old_callback = _host_lv_set_y_animation;
        old_end = -HOST_LV_VER_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_OUT_BOTTOM:
        old_callback = _host_lv_set_y_animation;
        old_end = HOST_LV_VER_RES;
        break;
    case LV_SCREEN_LOAD_ANIM_NONE:
    default:
        break;
    }

    _host_lv_configure_animation(&s_transition.new_animation, new_screen,
                                 new_callback, new_start, new_end,
                                 time, delay);
    s_transition.new_animation_live = true;
    if (old_screen != NULL && old_screen->live && old_callback != NULL)
    {
        _host_lv_configure_animation(&s_transition.old_animation, old_screen,
                                     old_callback, old_start, old_end,
                                     time, delay);
        s_transition.old_animation_live = true;
    }
}

void host_lv_reset(void)
{
    _host_lv_release_snapshot_allocations();
    memset(&s_initial_screen, 0, sizeof(s_initial_screen));
    s_initial_screen.live = true;
    s_initial_screen.kind = HOST_LV_OBJECT_SCREEN;
    s_initial_screen.width = HOST_LV_HOR_RES;
    s_initial_screen.height = HOST_LV_VER_RES;
    s_initial_screen.opacity = LV_OPA_COVER;
    s_initial_screen.text_opacity = LV_OPA_COVER;
    memset(&s_top_layer, 0, sizeof(s_top_layer));
    s_top_layer.live = true;
    s_top_layer.kind = HOST_LV_OBJECT_LAYER;
    s_top_layer.width = HOST_LV_HOR_RES;
    s_top_layer.height = HOST_LV_VER_RES;
    s_top_layer.opacity = LV_OPA_COVER;
    s_top_layer.text_opacity = LV_OPA_COVER;
    memset(&s_sys_layer, 0, sizeof(s_sys_layer));
    s_sys_layer.live = true;
    s_sys_layer.kind = HOST_LV_OBJECT_LAYER;
    s_sys_layer.width = HOST_LV_HOR_RES;
    s_sys_layer.height = HOST_LV_VER_RES;
    s_sys_layer.opacity = LV_OPA_COVER;
    s_sys_layer.text_opacity = LV_OPA_COVER;
    s_active_screen = &s_initial_screen;
    memset(&s_pointer_indev, 0, sizeof(s_pointer_indev));
    s_pointer_indev.state = LV_INDEV_STATE_RELEASED;
    s_event_indev = NULL;
    memset(s_objects, 0, sizeof(s_objects));
    memset(s_timers, 0, sizeof(s_timers));
    memset(&s_transition, 0, sizeof(s_transition));
    memset(s_generic_animations, 0, sizeof(s_generic_animations));
    memset(s_generic_animation_completion_values, 0,
           sizeof(s_generic_animation_completion_values));
    s_generic_animation_start_count = 0U;
    s_generic_animation_completion_count = 0U;
    memset(s_snapshot_captures, 0, sizeof(s_snapshot_captures));
    s_snapshot_capture_count = 0U;
    memset(&s_snapshot_failures, 0, sizeof(s_snapshot_failures));
    memset(s_snapshot_allocation_calls, 0,
           sizeof(s_snapshot_allocation_calls));
    s_snapshot_allocation_call_count = 0U;
    s_z_order = 0U;
    s_qrcode_update_count = 0U;
    s_qrcode_scrubbed_delete_count = 0U;
    memset(&s_chart_series, 0, sizeof(s_chart_series));
}

lv_indev_t *host_lv_pointer_indev(void)
{
    return &s_pointer_indev;
}

lv_obj_t *lv_screen_active(void)
{
    return s_active_screen;
}

lv_display_t *lv_display_get_default(void)
{
    return &s_display;
}

lv_obj_t *lv_display_get_screen_active(lv_display_t *display)
{
    return display == NULL || display == &s_display ? s_active_screen : NULL;
}

lv_obj_t *lv_display_get_layer_top(lv_display_t *display)
{
    return display == NULL || display == &s_display ? &s_top_layer : NULL;
}

lv_obj_t *lv_display_get_layer_sys(lv_display_t *display)
{
    return display == NULL || display == &s_display ? &s_sys_layer : NULL;
}

int32_t lv_display_get_horizontal_resolution(const lv_display_t *display)
{
    return display == NULL || display == &s_display ? HOST_LV_HOR_RES : 0;
}

int32_t lv_display_get_vertical_resolution(const lv_display_t *display)
{
    return display == NULL || display == &s_display ? HOST_LV_VER_RES : 0;
}

void lv_screen_load(lv_obj_t *screen)
{
    if (s_snapshot_failures.screen_loads > 0U)
    {
        --s_snapshot_failures.screen_loads;
        return;
    }
    if (s_snapshot_failures.screen_load_after_enabled)
    {
        if (s_snapshot_failures.screen_load_after == 0U)
        {
            s_snapshot_failures.screen_load_after_enabled = false;
            return;
        }
        --s_snapshot_failures.screen_load_after;
    }
    if (s_transition.pending)
    {
        _host_lv_complete_transition();
    }
    _host_lv_load_immediate(screen, false);
}

void lv_screen_load_anim(lv_obj_t *screen, lv_screen_load_anim_t animation,
                         uint32_t time, uint32_t delay, bool auto_delete)
{
    if (screen == NULL || !screen->live ||
            screen->kind != HOST_LV_OBJECT_SCREEN ||
            screen == s_active_screen ||
            (s_transition.pending && screen == s_transition.new_screen))
    {
        return;
    }
    if (s_transition.pending)
    {
        _host_lv_complete_transition();
    }
    _host_lv_normalize_screen(screen);
    _host_lv_normalize_screen(s_active_screen);
    if (time == 0U && delay == 0U)
    {
        _host_lv_load_immediate(screen, auto_delete);
        return;
    }
    if (s_snapshot_failures.native_animation_starts > 0U)
    {
        --s_snapshot_failures.native_animation_starts;
        return;
    }

    s_transition = (host_lv_transition_t)
    {
        .pending = true,
        .auto_delete = auto_delete,
        .old_screen = s_active_screen,
        .new_screen = screen,
        .animation = animation,
    };
    if (s_active_screen != NULL && s_active_screen->live)
    {
        (void)_host_lv_emit(s_active_screen,
                            LV_EVENT_SCREEN_UNLOAD_START);
    }
    _host_lv_prepare_transition_animations(time, delay);
}

lv_obj_t *lv_layer_top(void)
{
    return &s_top_layer;
}

lv_obj_t *lv_layer_sys(void)
{
    return &s_sys_layer;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    if (parent == NULL && s_snapshot_failures.screen_creations > 0U)
    {
        --s_snapshot_failures.screen_creations;
        return NULL;
    }
    const host_lv_object_kind_t kind = parent == NULL ?
                                       HOST_LV_OBJECT_SCREEN :
                                       HOST_LV_OBJECT_GENERIC;
    return _host_lv_allocate_object(kind, parent);
}

lv_obj_t *lv_button_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_BUTTON, parent);
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    lv_obj_t *label = _host_lv_allocate_object(HOST_LV_OBJECT_LABEL, parent);
    if (label != NULL)
    {
        label->label_long_mode = LV_LABEL_LONG_WRAP;
    }
    return label;
}

lv_obj_t *lv_slider_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_SLIDER, parent);
}

lv_obj_t *lv_switch_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_SWITCH, parent);
}

lv_obj_t *lv_bar_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_BAR, parent);
}

lv_obj_t *lv_chart_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_CHART, parent);
}

lv_obj_t *lv_canvas_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_CANVAS, parent);
}

lv_obj_t *lv_qrcode_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_QRCODE, parent);
}

void lv_qrcode_set_size(lv_obj_t *object, int32_t size)
{
    if (object != NULL && object->live &&
            object->kind == HOST_LV_OBJECT_QRCODE)
    {
        object->width = size;
        object->height = size;
    }
}

void lv_qrcode_set_dark_color(lv_obj_t *object, lv_color_t color)
{
    (void)object;
    (void)color;
}

void lv_qrcode_set_light_color(lv_obj_t *object, lv_color_t color)
{
    (void)object;
    (void)color;
}

void lv_qrcode_set_quiet_zone(lv_obj_t *object, bool enabled)
{
    (void)object;
    (void)enabled;
}

lv_result_t lv_qrcode_update(lv_obj_t *object, const void *data,
                             uint32_t data_length)
{
    if (object == NULL || !object->live ||
            object->kind != HOST_LV_OBJECT_QRCODE || data == NULL ||
            data_length == 0U)
    {
        return LV_RESULT_INVALID;
    }
    object->qrcode_data_length = data_length;
    object->qrcode_scrubbed = false;
    ++s_qrcode_update_count;
    return LV_RESULT_OK;
}

lv_obj_t *lv_image_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_IMAGE, parent);
}

void lv_canvas_set_draw_buf(lv_obj_t *canvas, lv_draw_buf_t *draw_buf)
{
    if (canvas == NULL || !canvas->live ||
            canvas->kind != HOST_LV_OBJECT_CANVAS || draw_buf == NULL)
    {
        return;
    }

    canvas->draw_buf = draw_buf;
    canvas->width = (int32_t)draw_buf->header.w;
    canvas->height = (int32_t)draw_buf->header.h;
}

void lv_canvas_fill_bg(lv_obj_t *canvas, lv_color_t color, lv_opa_t opacity)
{
    (void)color;
    (void)opacity;
    if (canvas == NULL || !canvas->live)
    {
        return;
    }
    if (canvas->kind == HOST_LV_OBJECT_QRCODE)
    {
        canvas->qrcode_data_length = 0U;
        canvas->qrcode_scrubbed = true;
    }
}

void lv_image_set_src(lv_obj_t *image, const void *source)
{
    if (image == NULL || !image->live ||
            image->kind != HOST_LV_OBJECT_IMAGE)
    {
        return;
    }

    image->image_source = source;
    const lv_draw_buf_t *draw_buf = source;
    if (draw_buf != NULL && draw_buf->data != NULL &&
            draw_buf->header.w > 0U && draw_buf->header.h > 0U)
    {
        image->width = (int32_t)draw_buf->header.w;
        image->height = (int32_t)draw_buf->header.h;
    }
}

lv_result_t lv_draw_buf_init(lv_draw_buf_t *draw_buf, uint32_t width,
                             uint32_t height, lv_color_format_t color_format,
                             uint32_t stride, void *data, uint32_t data_size)
{
    if (s_snapshot_failures.draw_buffer_initializations > 0U)
    {
        --s_snapshot_failures.draw_buffer_initializations;
        return LV_RESULT_INVALID;
    }
    if (draw_buf == NULL || data == NULL || width == 0U || height == 0U)
    {
        return LV_RESULT_INVALID;
    }

    if (stride == LV_STRIDE_AUTO)
    {
        stride = LV_DRAW_BUF_STRIDE(width, color_format);
    }
    if (stride == 0U || (uint64_t)stride * height > data_size)
    {
        return LV_RESULT_INVALID;
    }

    *draw_buf = (lv_draw_buf_t)
    {
        .header =
        {
            .cf = color_format,
            .w = width,
            .h = height,
            .stride = stride,
        },
        .data_size = data_size,
        .data = data,
        .unaligned_data = data,
    };
    return LV_RESULT_OK;
}

uint32_t lv_draw_buf_width_to_stride(uint32_t width,
                                     lv_color_format_t color_format)
{
    return LV_DRAW_BUF_STRIDE(width, color_format);
}

void lv_draw_buf_flush_cache(lv_draw_buf_t *draw_buf, const void *area)
{
    (void)area;
    if (draw_buf != NULL)
    {
        draw_buf->flush_count++;
    }
}

lv_result_t lv_snapshot_take_to_draw_buf(lv_obj_t *object,
        lv_color_format_t color_format,
        lv_draw_buf_t *draw_buf)
{
    if (s_snapshot_capture_count < HOST_LV_SNAPSHOT_CAPTURE_CAPACITY)
    {
        s_snapshot_captures[s_snapshot_capture_count] =
            (host_lv_snapshot_capture_t)
        {
            .object = object,
            .input_blocked = _host_lv_input_is_blocked(),
            .object_active = object == s_active_screen,
        };
    }
    ++s_snapshot_capture_count;

    if (s_snapshot_failures.captures > 0U)
    {
        --s_snapshot_failures.captures;
        return LV_RESULT_INVALID;
    }
    if (s_snapshot_failures.capture_after_enabled)
    {
        if (s_snapshot_failures.capture_after == 0U)
        {
            s_snapshot_failures.capture_after_enabled = false;
            return LV_RESULT_INVALID;
        }
        --s_snapshot_failures.capture_after;
    }
    if (!lv_obj_is_valid(object) || draw_buf == NULL || draw_buf->data == NULL ||
            color_format != LV_COLOR_FORMAT_RGB565)
    {
        return LV_RESULT_INVALID;
    }

    const uint32_t width = (uint32_t)lv_obj_get_width(object);
    const uint32_t height = (uint32_t)lv_obj_get_height(object);
    const uint32_t stride = LV_DRAW_BUF_STRIDE(width, color_format);
    if (width == 0U || height == 0U ||
            (uint64_t)stride * height > draw_buf->data_size)
    {
        return LV_RESULT_INVALID;
    }

    draw_buf->header = (lv_image_header_t)
    {
        .cf = color_format,
        .w = width,
        .h = height,
        .stride = stride,
    };
    memset(draw_buf->data, (int)(s_snapshot_capture_count & UINT8_MAX),
           (size_t)stride * height);
    return LV_RESULT_OK;
}

void lv_obj_delete(lv_obj_t *object)
{
    if (object == NULL || object == &s_initial_screen ||
            object == &s_top_layer || object == &s_sys_layer ||
            !object->live)
    {
        return;
    }
    if (s_transition.pending)
    {
        if (object == s_transition.new_screen)
        {
            _host_lv_normalize_screen(s_transition.old_screen);
            s_active_screen = s_transition.old_screen;
            memset(&s_transition, 0, sizeof(s_transition));
        }
        else if (object == s_transition.old_screen)
        {
            s_transition.old_screen = NULL;
            s_transition.old_animation_live = false;
        }
    }
    if (object == s_active_screen)
    {
        s_active_screen = NULL;
    }
    if (object->kind == HOST_LV_OBJECT_QRCODE &&
            object->qrcode_scrubbed && object->qrcode_data_length == 0U)
    {
        ++s_qrcode_scrubbed_delete_count;
    }
    _host_lv_delete_object(object);
}

void lv_obj_clean(lv_obj_t *object)
{
    if (object == NULL || !object->live)
    {
        return;
    }
    _host_lv_reset_pointer_target(object);
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        if (s_objects[index].live &&
                _host_lv_is_descendant(&s_objects[index], object))
        {
            s_objects[index].live = false;
        }
    }
}

lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *object,
                                    lv_event_cb_t callback,
                                    lv_event_code_t code, void *user_data)
{
    if (object == NULL || !object->live || callback == NULL)
    {
        return NULL;
    }
    for (size_t index = 0; index < HOST_LV_EVENT_CAPACITY; ++index)
    {
        lv_event_dsc_t *descriptor = &object->bindings[index];
        if (!descriptor->live)
        {
            *descriptor = (lv_event_dsc_t)
            {
                .live = true,
                .callback = callback,
                .code = code,
                .user_data = user_data,
            };
            ++object->binding_count;
            return descriptor;
        }
    }
    return NULL;
}

bool lv_obj_remove_event_dsc(lv_obj_t *object, lv_event_dsc_t *descriptor)
{
    if (object == NULL || !object->live || descriptor == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < HOST_LV_EVENT_CAPACITY; ++index)
    {
        if (descriptor == &object->bindings[index] && descriptor->live)
        {
            memset(descriptor, 0, sizeof(*descriptor));
            --object->binding_count;
            return true;
        }
    }
    return false;
}

void lv_obj_add_flag(lv_obj_t *object, lv_obj_flag_t flags)
{
    if (object != NULL && object->live)
    {
        object->flags |= flags;
    }
}

void lv_obj_remove_flag(lv_obj_t *object, lv_obj_flag_t flags)
{
    if (object != NULL && object->live)
    {
        object->flags &= ~flags;
    }
}

bool lv_obj_has_flag(const lv_obj_t *object, lv_obj_flag_t flags)
{
    return object != NULL && object->live &&
           (object->flags & flags) == flags;
}

void lv_obj_set_user_data(lv_obj_t *object, void *user_data)
{
    if (object != NULL && object->live)
    {
        object->user_data = user_data;
    }
}

void *lv_obj_get_user_data(lv_obj_t *object)
{
    return object != NULL && object->live ? object->user_data : NULL;
}

void lv_obj_add_state(lv_obj_t *object, lv_state_t state)
{
    if (object != NULL && object->live)
    {
        object->state |= state;
    }
}

void lv_obj_remove_state(lv_obj_t *object, lv_state_t state)
{
    if (object != NULL && object->live)
    {
        object->state &= ~state;
    }
}

bool lv_obj_has_state(const lv_obj_t *object, lv_state_t state)
{
    return object != NULL && object->live &&
           (object->state & state) == state;
}

bool lv_obj_is_valid(const lv_obj_t *object)
{
    if (object == &s_initial_screen || object == &s_top_layer ||
            object == &s_sys_layer)
    {
        return object->live;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        if (object == &s_objects[index])
        {
            return object->live;
        }
    }
    return false;
}

lv_obj_t *lv_obj_get_parent(const lv_obj_t *object)
{
    return lv_obj_is_valid(object) ? object->parent : NULL;
}

lv_display_t *lv_obj_get_display(const lv_obj_t *object)
{
    return lv_obj_is_valid(object) ? &s_display : NULL;
}

uint32_t lv_obj_get_child_count(const lv_obj_t *object)
{
    const size_t count = host_lv_object_child_count(object);
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

lv_obj_t *lv_obj_get_child(const lv_obj_t *object, int32_t index)
{
    const size_t count = host_lv_object_child_count(object);
    int64_t resolved_index = index;

    if (index < 0)
    {
        resolved_index = (int64_t)count + index;
    }
    if (resolved_index < 0 || (uint64_t)resolved_index >= count)
    {
        return NULL;
    }

    const lv_obj_t *child = NULL;
    return _host_lv_child_at(object, (size_t)resolved_index, &child) ?
           (lv_obj_t *)child : NULL;
}

void lv_obj_update_layout(const lv_obj_t *object)
{
    (void)object;
}

void lv_obj_move_to_index(lv_obj_t *object, int32_t index)
{
    if (!lv_obj_is_valid(object) || object->parent == NULL)
    {
        return;
    }

    lv_obj_t *siblings[HOST_LV_OBJECT_CAPACITY];
    size_t sibling_count = 0U;
    for (size_t object_index = 0; object_index < HOST_LV_OBJECT_CAPACITY;
            ++object_index)
    {
        lv_obj_t *candidate = &s_objects[object_index];
        if (candidate->live && candidate->parent == object->parent)
        {
            size_t insert_index = sibling_count;
            while (insert_index > 0U &&
                    siblings[insert_index - 1U]->z_order >
                    candidate->z_order)
            {
                siblings[insert_index] = siblings[insert_index - 1U];
                --insert_index;
            }
            siblings[insert_index] = candidate;
            ++sibling_count;
        }
    }
    if (sibling_count < 2U)
    {
        return;
    }

    size_t old_index = 0U;
    while (old_index < sibling_count && siblings[old_index] != object)
    {
        ++old_index;
    }
    if (old_index == sibling_count)
    {
        return;
    }
    for (size_t move_index = old_index; move_index + 1U < sibling_count;
            ++move_index)
    {
        siblings[move_index] = siblings[move_index + 1U];
    }

    size_t new_index;
    if (index < 0)
    {
        const int64_t translated = (int64_t)sibling_count + index;
        new_index = translated < 0 ? 0U : (size_t)translated;
    }
    else
    {
        new_index = (size_t)index;
    }
    if (new_index >= sibling_count)
    {
        new_index = sibling_count - 1U;
    }
    for (size_t move_index = sibling_count - 1U;
            move_index > new_index; --move_index)
    {
        siblings[move_index] = siblings[move_index - 1U];
    }
    siblings[new_index] = object;
    for (size_t sibling_index = 0; sibling_index < sibling_count;
            ++sibling_index)
    {
        siblings[sibling_index]->z_order = ++s_z_order;
    }
}

void lv_obj_set_pos(lv_obj_t *object, int32_t x, int32_t y)
{
    if (object != NULL && object->live)
    {
        object->x = x;
        object->y = y;
    }
}

void lv_obj_set_x(lv_obj_t *object, int32_t x)
{
    if (object != NULL && object->live)
    {
        object->x = x;
    }
}

void lv_obj_set_y(lv_obj_t *object, int32_t y)
{
    if (object != NULL && object->live)
    {
        object->y = y;
    }
}

int32_t lv_obj_get_x(const lv_obj_t *object)
{
    return object == NULL || !object->live ? 0 : object->x;
}

int32_t lv_obj_get_y(const lv_obj_t *object)
{
    return object == NULL || !object->live ? 0 : object->y;
}

void lv_obj_set_size(lv_obj_t *object, int32_t width, int32_t height)
{
    if (object != NULL && object->live)
    {
        object->width = _host_lv_resolve_dimension(object, width, true);
        object->height = _host_lv_resolve_dimension(object, height, false);
    }
}

void lv_obj_set_width(lv_obj_t *object, int32_t width)
{
    if (object != NULL && object->live)
    {
        object->width = _host_lv_resolve_dimension(object, width, true);
    }
}

void lv_obj_set_height(lv_obj_t *object, int32_t height)
{
    if (object != NULL && object->live)
    {
        object->height = _host_lv_resolve_dimension(object, height, false);
    }
}

int32_t lv_obj_get_width(const lv_obj_t *object)
{
    return object == NULL || !object->live ? 0 : object->width;
}

int32_t lv_obj_get_height(const lv_obj_t *object)
{
    return object == NULL || !object->live ? 0 : object->height;
}

void lv_obj_set_style_opa(lv_obj_t *object, lv_opa_t opacity,
                          lv_style_selector_t selector)
{
    (void)selector;
    if (object != NULL && object->live)
    {
        object->opacity = opacity;
    }
}

lv_opa_t lv_obj_get_style_opa(const lv_obj_t *object, lv_part_t part)
{
    (void)part;
    return object == NULL || !object->live ? LV_OPA_TRANSP : object->opacity;
}

void lv_obj_set_style_bg_opa(lv_obj_t *object, lv_opa_t opacity,
                             lv_style_selector_t selector)
{
    (void)selector;
    if (object != NULL && object->live)
    {
        object->background_opacity = opacity;
    }
}

lv_opa_t lv_obj_get_style_bg_opa(const lv_obj_t *object, lv_part_t part)
{
    (void)part;
    return object == NULL || !object->live ? LV_OPA_TRANSP :
           object->background_opacity;
}

void lv_obj_set_style_text_opa(lv_obj_t *object, lv_opa_t opacity,
                               lv_style_selector_t selector)
{
    (void)selector;
    if (object != NULL && object->live)
    {
        object->text_opacity = opacity;
    }
}

void lv_obj_set_style_image_recolor(lv_obj_t *object, lv_color_t color,
                                    lv_style_selector_t selector)
{
    (void)selector;
    if (object != NULL && object->live)
    {
        object->image_recolor = color;
    }
}

void lv_obj_set_style_image_opa(lv_obj_t *object, lv_opa_t opacity,
                                lv_style_selector_t selector)
{
    (void)selector;
    if (object != NULL && object->live)
    {
        object->image_opacity = opacity;
    }
}

void lv_obj_remove_style_all(lv_obj_t *object)
{
    if (object != NULL && object->live)
    {
        object->opacity = LV_OPA_COVER;
        object->background_opacity = LV_OPA_TRANSP;
        object->text_opacity = LV_OPA_COVER;
        object->image_recolor = 0U;
        object->image_opacity = LV_OPA_COVER;
    }
}

void lv_obj_invalidate(lv_obj_t *object)
{
    if (object != NULL && object->live)
    {
        object->invalidation_count++;
    }
}

lv_event_code_t lv_event_get_code(lv_event_t *event)
{
    return event == NULL ? 0 : event->code;
}

void *lv_event_get_user_data(lv_event_t *event)
{
    return event == NULL ? NULL : event->user_data;
}

lv_obj_t *lv_event_get_target(lv_event_t *event)
{
    return event == NULL ? NULL : event->target;
}

lv_obj_t *lv_event_get_target_obj(lv_event_t *event)
{
    return lv_event_get_target(event);
}

lv_obj_t *lv_event_get_current_target_obj(lv_event_t *event)
{
    return lv_event_get_target(event);
}

lv_indev_t *lv_event_get_indev(lv_event_t *event)
{
    return event == NULL ? NULL : event->indev;
}

void *lv_event_get_param(lv_event_t *event)
{
    return event == NULL ? NULL : event->param;
}

lv_indev_t *lv_indev_active(void)
{
    return s_event_indev;
}

void lv_indev_get_point(const lv_indev_t *indev, lv_point_t *point)
{
    if (point == NULL)
    {
        return;
    }
    *point = indev == &s_pointer_indev ? indev->point : (lv_point_t)
    {
        .x = 0,
        .y = 0,
    };
}

lv_indev_state_t lv_indev_get_state(const lv_indev_t *indev)
{
    return indev == &s_pointer_indev ? indev->state :
           LV_INDEV_STATE_RELEASED;
}

void lv_indev_reset(lv_indev_t *indev, lv_obj_t *object)
{
    if (indev != NULL && indev != &s_pointer_indev)
    {
        return;
    }
    if (object != NULL && object != s_pointer_indev.active_object)
    {
        return;
    }
    _host_lv_reset_pointer_target(object);
}

void lv_indev_wait_release(lv_indev_t *indev)
{
    if (indev == &s_pointer_indev && indev->sequence_active)
    {
        indev->wait_release = true;
    }
}

void lv_label_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || !label->live)
    {
        return;
    }
    snprintf(label->text, sizeof(label->text), "%s", text == NULL ? "" : text);
}

void lv_label_set_text_fmt(lv_obj_t *label, const char *format, ...)
{
    if (label == NULL || !label->live || format == NULL)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)vsnprintf(label->text, sizeof(label->text), format, args);
    va_end(args);
}

void lv_label_set_long_mode(lv_obj_t *label, int mode)
{
    if (label != NULL && label->live &&
            label->kind == HOST_LV_OBJECT_LABEL)
    {
        label->label_long_mode = mode;
    }
}

void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font,
                                lv_style_selector_t selector)
{
    (void)selector;
    if (object != NULL && object->live)
    {
        object->text_font = font;
    }
}

void lv_bar_set_range(lv_obj_t *bar, int32_t minimum, int32_t maximum)
{
    if (bar != NULL && bar->live)
    {
        bar->slider_minimum = minimum;
        bar->slider_maximum = maximum;
    }
}

void lv_bar_set_value(lv_obj_t *bar, int32_t value, int animation)
{
    (void)animation;
    if (bar != NULL && bar->live)
    {
        bar->slider_value = value;
    }
}

void lv_bar_set_start_value(lv_obj_t *bar, int32_t value, int animation)
{
    (void)animation;
    if (bar != NULL && bar->live)
    {
        bar->bar_start_value = value;
    }
}

void lv_bar_set_mode(lv_obj_t *bar, int mode)
{
    (void)bar;
    (void)mode;
}

void lv_chart_set_type(lv_obj_t *chart, int type)
{
    (void)chart;
    (void)type;
}

void lv_chart_set_point_count(lv_obj_t *chart, uint32_t count)
{
    (void)chart;
    (void)count;
}

void lv_chart_set_axis_range(lv_obj_t *chart, int axis, int32_t minimum,
                             int32_t maximum)
{
    (void)axis;
    if (chart != NULL && chart->live)
    {
        chart->slider_minimum = minimum;
        chart->slider_maximum = maximum;
    }
}

void lv_chart_set_div_line_count(lv_obj_t *chart, uint32_t horizontal,
                                 uint32_t vertical)
{
    (void)chart;
    (void)horizontal;
    (void)vertical;
}

lv_chart_series_t *lv_chart_add_series(lv_obj_t *chart, lv_color_t color,
                                       int axis)
{
    (void)color;
    (void)axis;
    if (chart == NULL || !chart->live)
    {
        return NULL;
    }
    s_chart_series.chart = chart;
    return &s_chart_series;
}

void lv_chart_set_next_value(lv_obj_t *chart, lv_chart_series_t *series,
                             int32_t value)
{
    if (chart != NULL && chart->live && series != NULL &&
            series->chart == chart)
    {
        series->last_value = value;
    }
}

void lv_slider_set_range(lv_obj_t *slider, int32_t minimum, int32_t maximum)
{
    if (slider != NULL && slider->live)
    {
        slider->slider_minimum = minimum;
        slider->slider_maximum = maximum;
    }
}

void lv_slider_set_value(lv_obj_t *slider, int32_t value, int animation)
{
    (void)animation;
    if (slider != NULL && slider->live)
    {
        slider->slider_value = value;
    }
}

int32_t lv_slider_get_value(lv_obj_t *slider)
{
    return slider == NULL ? 0 : slider->slider_value;
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period,
                            void *user_data)
{
    for (size_t index = 0; index < HOST_LV_TIMER_CAPACITY; ++index)
    {
        if (!s_timers[index].live)
        {
            s_timers[index] = (lv_timer_t)
            {
                .live = true,
                .callback = callback,
                .period = period,
                .user_data = user_data,
            };
            return &s_timers[index];
        }
    }
    return NULL;
}

void lv_timer_delete(lv_timer_t *timer)
{
    if (timer != NULL)
    {
        memset(timer, 0, sizeof(*timer));
    }
}

void lv_timer_pause(lv_timer_t *timer)
{
    if (timer != NULL && timer->live)
    {
        timer->paused = true;
    }
}

void lv_timer_resume(lv_timer_t *timer)
{
    if (timer != NULL && timer->live)
    {
        timer->paused = false;
    }
}

void lv_timer_ready(lv_timer_t *timer)
{
    if (timer != NULL && timer->live)
    {
        timer->ready = true;
    }
}

void *lv_timer_get_user_data(lv_timer_t *timer)
{
    return timer == NULL ? NULL : timer->user_data;
}

lv_anim_t *lv_anim_get(void *variable, lv_anim_exec_xcb_t execute_callback)
{
    if (s_transition.new_animation_live &&
            s_transition.new_animation.var == variable &&
            (execute_callback == NULL ||
             s_transition.new_animation.exec_cb == execute_callback))
    {
        return &s_transition.new_animation;
    }
    if (s_transition.old_animation_live &&
            s_transition.old_animation.var == variable &&
            (execute_callback == NULL ||
             s_transition.old_animation.exec_cb == execute_callback))
    {
        return &s_transition.old_animation;
    }
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        host_lv_generic_animation_t *generic = &s_generic_animations[index];
        if (generic->live && generic->animation.var == variable &&
                (execute_callback == NULL ||
                 generic->animation.exec_cb == execute_callback))
        {
            return &generic->animation;
        }
    }
    return NULL;
}

bool lv_anim_delete(void *variable, lv_anim_exec_xcb_t execute_callback)
{
    bool deleted = false;

    if (s_transition.new_animation_live &&
            s_transition.new_animation.var == variable &&
            (execute_callback == NULL ||
             s_transition.new_animation.exec_cb == execute_callback))
    {
        s_transition.new_animation_live = false;
        deleted = true;
    }
    if (s_transition.old_animation_live &&
            s_transition.old_animation.var == variable &&
            (execute_callback == NULL ||
             s_transition.old_animation.exec_cb == execute_callback))
    {
        s_transition.old_animation_live = false;
        deleted = true;
    }
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        host_lv_generic_animation_t *generic = &s_generic_animations[index];
        if (generic->live && generic->animation.var == variable &&
                (execute_callback == NULL ||
                 generic->animation.exec_cb == execute_callback))
        {
            generic->live = false;
            deleted = true;
        }
    }
    return deleted;
}

void lv_anim_init(lv_anim_t *animation)
{
    if (animation != NULL)
    {
        memset(animation, 0, sizeof(*animation));
    }
}

void lv_anim_set_var(lv_anim_t *animation, void *variable)
{
    if (animation != NULL)
    {
        animation->var = variable;
    }
}

void lv_anim_set_exec_cb(lv_anim_t *animation,
                         lv_anim_exec_xcb_t execute_callback)
{
    if (animation != NULL)
    {
        animation->exec_cb = execute_callback;
    }
}

void lv_anim_set_values(lv_anim_t *animation, int32_t start_value,
                        int32_t end_value)
{
    if (animation != NULL)
    {
        animation->start_value = start_value;
        animation->current_value = start_value;
        animation->end_value = end_value;
    }
}

void lv_anim_set_duration(lv_anim_t *animation, uint32_t duration)
{
    if (animation != NULL)
    {
        animation->duration = (int32_t)duration;
    }
}

void lv_anim_set_completed_cb(lv_anim_t *animation,
                              lv_anim_completed_cb_t completed_callback)
{
    if (animation != NULL)
    {
        animation->completed_cb = completed_callback;
    }
}

void lv_anim_set_early_apply(lv_anim_t *animation, bool enabled)
{
    if (animation != NULL)
    {
        animation->early_apply = enabled;
    }
}

lv_anim_t *lv_anim_start(const lv_anim_t *animation)
{
    if (animation == NULL || animation->var == NULL ||
            animation->exec_cb == NULL || animation->duration < 0)
    {
        return NULL;
    }
    if (s_snapshot_failures.animation_starts > 0U)
    {
        --s_snapshot_failures.animation_starts;
        return NULL;
    }

    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        host_lv_generic_animation_t *generic = &s_generic_animations[index];
        if (!generic->live)
        {
            generic->live = true;
            generic->animation = *animation;
            generic->animation.act_time = 0;
            generic->animation.current_value = animation->start_value;
            ++s_generic_animation_start_count;
            if (generic->animation.early_apply)
            {
                generic->animation.exec_cb(generic->animation.var,
                                           generic->animation.start_value);
            }
            return &generic->animation;
        }
    }
    return NULL;
}

static void _host_lv_run_ready_timers(void)
{
    for (size_t index = 0; index < HOST_LV_TIMER_CAPACITY; ++index)
    {
        lv_timer_t *timer = &s_timers[index];
        if (timer->live && !timer->paused && timer->ready &&
                timer->callback != NULL)
        {
            timer->ready = false;
            timer->callback(timer);
        }
    }
}

void host_lv_timer_step(void)
{
    for (size_t index = 0; index < HOST_LV_TIMER_CAPACITY; ++index)
    {
        if (s_timers[index].live && !s_timers[index].paused)
        {
            s_timers[index].ready = true;
        }
    }
    _host_lv_run_ready_timers();
}

static void _host_lv_refresh_generic_animations(void)
{
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        host_lv_generic_animation_t *generic = &s_generic_animations[index];
        if (!generic->live)
        {
            continue;
        }

        lv_anim_t *animation = &generic->animation;
        const int32_t duration = animation->duration;
        const int32_t elapsed = animation->act_time > duration ?
                                duration : animation->act_time;
        const int32_t value = duration == 0 ? animation->end_value :
                              animation->start_value +
                              (int32_t)(((int64_t)(animation->end_value -
                                         animation->start_value) *
                                         elapsed) / duration);
        animation->current_value = value;
        animation->exec_cb(animation->var, value);
        if (animation->act_time >= duration)
        {
            if (s_generic_animation_completion_count <
                    HOST_LV_GENERIC_ANIM_COMPLETION_CAPACITY)
            {
                s_generic_animation_completion_values[
                    s_generic_animation_completion_count] = value;
            }
            ++s_generic_animation_completion_count;
            lv_anim_completed_cb_t completed_callback = animation->completed_cb;
            generic->live = false;
            if (completed_callback != NULL)
            {
                completed_callback(animation);
            }
        }
    }
}

void lv_anim_refr_now(void)
{
    if (s_transition.pending)
    {
        lv_anim_t *new_animation = &s_transition.new_animation;
        if (!s_transition.started && new_animation->act_time >= 0)
        {
            s_transition.started = true;
            s_active_screen = s_transition.new_screen;
            (void)_host_lv_emit(s_transition.new_screen,
                                LV_EVENT_SCREEN_LOAD_START);
        }
        if (new_animation->act_time >= 0)
        {
            const int32_t duration = new_animation->duration;
            const int32_t time = new_animation->act_time > duration ?
                                 duration : new_animation->act_time;
            const int32_t value = duration == 0 ? new_animation->end_value :
                                  new_animation->start_value +
                                  (int32_t)(((int64_t)(new_animation->end_value -
                                             new_animation->start_value) *
                                             time) / duration);
            new_animation->current_value = value;
            new_animation->exec_cb(new_animation->var, value);
        }
        if (new_animation->act_time >= new_animation->duration)
        {
            _host_lv_complete_transition();
        }
    }
    _host_lv_refresh_generic_animations();
    _host_lv_run_ready_timers();
}

void app_manager_host_ui_step(void)
{
    if (s_transition.pending && s_transition.new_animation_live)
    {
        lv_anim_t *animation = &s_transition.new_animation;
        animation->act_time += HOST_LV_ANIM_STEP_MS;
        if (s_transition.old_animation_live)
        {
            s_transition.old_animation.act_time += HOST_LV_ANIM_STEP_MS;
        }
    }
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        if (s_generic_animations[index].live)
        {
            s_generic_animations[index].animation.act_time +=
                HOST_LV_ANIM_STEP_MS;
        }
    }
    lv_anim_refr_now();
}

static void _host_lv_set_x_animation(void *variable, int32_t value)
{
    lv_obj_set_x(variable, value);
}

static void _host_lv_set_y_animation(void *variable, int32_t value)
{
    lv_obj_set_y(variable, value);
}

static void _host_lv_set_opacity_animation(void *variable, int32_t value)
{
    lv_obj_set_style_opa(variable, (lv_opa_t)value, 0);
}

void host_lv_snapshot_fail_next_allocations(unsigned count)
{
    s_snapshot_failures.allocations = count;
}

void host_lv_snapshot_fail_allocation_after(unsigned successful_allocations)
{
    s_snapshot_failures.allocation_after = successful_allocations;
    s_snapshot_failures.allocation_after_enabled = true;
}

void host_lv_snapshot_fail_next_draw_buffer_initializations(unsigned count)
{
    s_snapshot_failures.draw_buffer_initializations = count;
}

void host_lv_snapshot_fail_next_captures(unsigned count)
{
    s_snapshot_failures.captures = count;
}

void host_lv_snapshot_fail_capture_after(unsigned successful_captures)
{
    s_snapshot_failures.capture_after = successful_captures;
    s_snapshot_failures.capture_after_enabled = true;
}

void host_lv_snapshot_fail_next_object_creations(unsigned count)
{
    s_snapshot_failures.object_creations = count;
}

void host_lv_snapshot_fail_object_creation_after(unsigned successful_creations)
{
    s_snapshot_failures.object_creation_after = successful_creations;
    s_snapshot_failures.object_creation_after_enabled = true;
}

void host_lv_snapshot_fail_next_animation_starts(unsigned count)
{
    s_snapshot_failures.animation_starts = count;
}

void host_lv_fail_next_screen_animation_starts(unsigned count)
{
    s_snapshot_failures.native_animation_starts = count;
}

void host_lv_fail_next_screen_creations(unsigned count)
{
    s_snapshot_failures.screen_creations = count;
}

void host_lv_snapshot_fail_next_screen_loads(unsigned count)
{
    s_snapshot_failures.screen_loads = count;
}

void host_lv_fail_screen_load_after(unsigned successful_loads)
{
    s_snapshot_failures.screen_load_after = successful_loads;
    s_snapshot_failures.screen_load_after_enabled = true;
}

size_t host_lv_snapshot_capture_count(void)
{
    return s_snapshot_capture_count;
}

bool host_lv_snapshot_capture_at(size_t index, const lv_obj_t **object,
                                 bool *input_blocked, bool *object_active)
{
    if (object == NULL || input_blocked == NULL || object_active == NULL ||
            index >= s_snapshot_capture_count ||
            index >= HOST_LV_SNAPSHOT_CAPTURE_CAPACITY)
    {
        return false;
    }
    *object = s_snapshot_captures[index].object;
    *input_blocked = s_snapshot_captures[index].input_blocked;
    *object_active = s_snapshot_captures[index].object_active;
    return true;
}

size_t host_lv_object_child_count(const lv_obj_t *parent)
{
    if (!lv_obj_is_valid(parent))
    {
        return 0U;
    }

    size_t count = 0U;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        count += s_objects[index].live && s_objects[index].parent == parent ?
                 1U : 0U;
    }
    return count;
}

static bool _host_lv_child_at(const lv_obj_t *parent, size_t child_index,
                              const lv_obj_t **child)
{
    const lv_obj_t *result = NULL;
    uint64_t lower_z_order = 0U;
    for (size_t ordinal = 0U; ; ++ordinal)
    {
        const lv_obj_t *next = NULL;
        for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
        {
            const lv_obj_t *candidate = &s_objects[index];
            if (candidate->live && candidate->parent == parent &&
                    candidate->z_order > lower_z_order &&
                    (next == NULL || candidate->z_order < next->z_order))
            {
                next = candidate;
            }
        }
        if (next == NULL)
        {
            return false;
        }
        if (ordinal == child_index)
        {
            result = next;
            break;
        }
        lower_z_order = next->z_order;
    }

    *child = result;
    return true;
}

bool host_lv_object_child_snapshot(const lv_obj_t *parent, size_t index,
                                   host_lv_object_snapshot_t *snapshot)
{
    const lv_obj_t *child = NULL;
    if (snapshot == NULL || !_host_lv_child_at(parent, index, &child))
    {
        return false;
    }

    *snapshot = (host_lv_object_snapshot_t)
    {
        .object = child,
        .parent = child->parent,
        .live = child->live,
        .image = child->kind == HOST_LV_OBJECT_IMAGE,
        .has_draw_buf_source = child->kind == HOST_LV_OBJECT_IMAGE &&
                               child->image_source != NULL,
                               .flags = child->flags,
                               .x = child->x,
                               .y = child->y,
                               .width = child->width,
                               .height = child->height,
                               .opacity = child->opacity,
                               .background_opacity = child->background_opacity,
    };
    return true;
}

size_t host_lv_generic_animation_count(void)
{
    size_t count = 0U;
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        count += s_generic_animations[index].live ? 1U : 0U;
    }
    return count;
}

void host_lv_advance_generic_animations(uint32_t elapsed_ms)
{
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        if (s_generic_animations[index].live)
        {
            const int64_t advanced =
                (int64_t)s_generic_animations[index].animation.act_time +
                elapsed_ms;
            s_generic_animations[index].animation.act_time =
                advanced > INT32_MAX ? INT32_MAX : (int32_t)advanced;
        }
    }
    lv_anim_refr_now();
}

void host_lv_complete_generic_animations(void)
{
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        if (s_generic_animations[index].live)
        {
            s_generic_animations[index].animation.act_time =
                s_generic_animations[index].animation.duration;
        }
    }
    lv_anim_refr_now();
}

void *host_lv_heap_caps_aligned_calloc(size_t alignment, size_t count,
                                       size_t size, uint32_t caps)
{
    const size_t call_index = s_snapshot_allocation_call_count++;
    if (call_index < HOST_LV_SNAPSHOT_ALLOCATION_CALL_CAPACITY)
    {
        s_snapshot_allocation_calls[call_index] = (host_lv_snapshot_allocation_call_t)
        {
            .alignment = alignment,
            .count = count,
            .size = size,
            .caps = caps,
            .succeeded = false,
        };
    }
    if (s_snapshot_failures.allocations > 0U)
    {
        --s_snapshot_failures.allocations;
        return NULL;
    }
    if (s_snapshot_failures.allocation_after_enabled)
    {
        if (s_snapshot_failures.allocation_after == 0U)
        {
            s_snapshot_failures.allocation_after_enabled = false;
            return NULL;
        }
        --s_snapshot_failures.allocation_after;
    }
    if (count == 0U || size == 0U || size > SIZE_MAX / count)
    {
        return NULL;
    }
    void *pointer = calloc(count, size);
    if (pointer == NULL)
    {
        return NULL;
    }
    if (!_host_lv_track_snapshot_allocation(pointer, count * size))
    {
        free(pointer);
        return NULL;
    }
    if (call_index < HOST_LV_SNAPSHOT_ALLOCATION_CALL_CAPACITY)
    {
        s_snapshot_allocation_calls[call_index].succeeded = true;
    }
    return pointer;
}

void host_lv_heap_caps_free(void *pointer)
{
    _host_lv_untrack_snapshot_allocation(pointer);
    free(pointer);
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

size_t host_lv_snapshot_live_allocation_count(void)
{
    size_t count = 0U;
    for (size_t index = 0U;
            index < HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY; ++index)
    {
        count += s_snapshot_allocations[index].pointer != NULL ? 1U : 0U;
    }
    return count;
}

size_t host_lv_snapshot_live_allocation_bytes(void)
{
    size_t bytes = 0U;
    for (size_t index = 0U;
            index < HOST_LV_SNAPSHOT_ALLOCATION_CAPACITY; ++index)
    {
        bytes += s_snapshot_allocations[index].size;
    }
    return bytes;
}

size_t host_lv_generic_animation_start_count(void)
{
    return s_generic_animation_start_count;
}

size_t host_lv_generic_animation_completion_count(void)
{
    return s_generic_animation_completion_count;
}

bool host_lv_generic_animation_completion_value_at(size_t index,
        int32_t *value)
{
    if (value == NULL || index >= s_generic_animation_completion_count ||
            index >= HOST_LV_GENERIC_ANIM_COMPLETION_CAPACITY)
    {
        return false;
    }
    *value = s_generic_animation_completion_values[index];
    return true;
}

size_t host_lv_snapshot_allocation_call_count(void)
{
    return s_snapshot_allocation_call_count;
}

bool host_lv_snapshot_allocation_call_at(
    size_t index, host_lv_snapshot_allocation_call_t *call)
{
    if (call == NULL || index >= s_snapshot_allocation_call_count ||
            index >= HOST_LV_SNAPSHOT_ALLOCATION_CALL_CAPACITY)
    {
        return false;
    }
    *call = s_snapshot_allocation_calls[index];
    return true;
}

bool host_lv_touch_press(int32_t x, int32_t y)
{
    if (s_pointer_indev.sequence_active)
    {
        return false;
    }

    s_pointer_indev.point = (lv_point_t)
    {
        .x = x,
        .y = y,
    };
    lv_obj_t *target = _host_lv_find_pointer_target(&s_pointer_indev.point);
    if (target == NULL)
    {
        s_pointer_indev.state = LV_INDEV_STATE_RELEASED;
        return false;
    }

    s_pointer_indev.state = LV_INDEV_STATE_PRESSED;
    s_pointer_indev.active_object = target;
    s_pointer_indev.sequence_active = true;
    s_pointer_indev.wait_release = false;
    (void)_host_lv_emit_with_input(target, LV_EVENT_PRESSED,
                                   &s_pointer_indev, NULL);
    return true;
}

bool host_lv_touch_move(int32_t x, int32_t y)
{
    if (!s_pointer_indev.sequence_active)
    {
        return false;
    }

    s_pointer_indev.point = (lv_point_t)
    {
        .x = x,
        .y = y,
    };
    lv_obj_t *target = s_pointer_indev.active_object;
    if (target == NULL || !target->live)
    {
        return false;
    }

    if ((target->flags & LV_OBJ_FLAG_PRESS_LOCK) != 0U ||
            _host_lv_contains_point(target, &s_pointer_indev.point))
    {
        (void)_host_lv_emit_with_input(target, LV_EVENT_PRESSING,
                                       &s_pointer_indev, NULL);
    }
    else
    {
        s_pointer_indev.active_object = NULL;
        (void)_host_lv_emit_with_input(target, LV_EVENT_PRESS_LOST,
                                       &s_pointer_indev, NULL);
    }
    return true;
}

bool host_lv_touch_release(int32_t x, int32_t y)
{
    if (!s_pointer_indev.sequence_active)
    {
        return false;
    }

    s_pointer_indev.point = (lv_point_t)
    {
        .x = x,
        .y = y,
    };
    lv_obj_t *target = s_pointer_indev.active_object;
    const bool wait_release = s_pointer_indev.wait_release;
    s_pointer_indev.active_object = NULL;
    s_pointer_indev.sequence_active = false;
    s_pointer_indev.wait_release = false;
    s_pointer_indev.state = LV_INDEV_STATE_RELEASED;
    if (target == NULL || !target->live || wait_release)
    {
        return true;
    }

    if ((target->flags & LV_OBJ_FLAG_PRESS_LOCK) != 0U ||
            _host_lv_contains_point(target, &s_pointer_indev.point))
    {
        (void)_host_lv_emit_with_input(target, LV_EVENT_RELEASED,
                                       &s_pointer_indev, NULL);
    }
    else
    {
        (void)_host_lv_emit_with_input(target, LV_EVENT_PRESS_LOST,
                                       &s_pointer_indev, NULL);
    }
    return true;
}

void host_lv_touch_reset(void)
{
    lv_indev_reset(&s_pointer_indev, NULL);
}

bool host_lv_visible_slider_drag(int32_t value)
{
    if (_host_lv_input_is_blocked())
    {
        return false;
    }
    lv_obj_t *slider = _host_lv_find_visible_slider(true);
    if (slider == NULL)
    {
        return false;
    }

    if ((slider->state & LV_STATE_PRESSED) == 0U)
    {
        slider->state |= LV_STATE_PRESSED;
        (void)_host_lv_emit(slider, LV_EVENT_PRESSED);
    }
    if (value < slider->slider_minimum)
    {
        value = slider->slider_minimum;
    }
    else if (value > slider->slider_maximum)
    {
        value = slider->slider_maximum;
    }
    slider->slider_value = value;
    return _host_lv_emit(slider, LV_EVENT_VALUE_CHANGED);
}

bool host_lv_visible_slider_release(void)
{
    if (_host_lv_input_is_blocked())
    {
        return false;
    }
    lv_obj_t *slider = _host_lv_find_visible_slider(true);
    if (slider == NULL ||
            (slider->state & LV_STATE_PRESSED) == 0U)
    {
        return false;
    }

    slider->state &= ~LV_STATE_PRESSED;
    return _host_lv_emit(slider, LV_EVENT_RELEASED);
}

bool host_lv_visible_slider_cancel(void)
{
    if (_host_lv_input_is_blocked())
    {
        return false;
    }
    lv_obj_t *slider = _host_lv_find_visible_slider(true);
    if (slider == NULL ||
            (slider->state & LV_STATE_PRESSED) == 0U)
    {
        return false;
    }

    slider->state &= ~LV_STATE_PRESSED;
    return _host_lv_emit(slider, LV_EVENT_PRESS_LOST);
}

bool host_lv_visible_slider_snapshot(int32_t *value, bool *pressed,
                                     bool *disabled)
{
    if (value == NULL || pressed == NULL || disabled == NULL)
    {
        return false;
    }
    const lv_obj_t *slider = _host_lv_find_visible_slider(false);
    if (slider == NULL)
    {
        return false;
    }
    *value = slider->slider_value;
    *pressed = (slider->state & LV_STATE_PRESSED) != 0U;
    *disabled = (slider->state & LV_STATE_DISABLED) != 0U;
    return true;
}

bool host_lv_click_action(const char *title)
{
    if (_host_lv_input_is_blocked())
    {
        return false;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        lv_obj_t *object = &s_objects[index];
        if (object->live && _host_lv_is_visible(object) &&
                object->kind == HOST_LV_OBJECT_BUTTON &&
                (object->state & LV_STATE_DISABLED) == 0U &&
                _host_lv_has_descendant_text(object, title) &&
                _host_lv_emit(object, LV_EVENT_CLICKED))
        {
            return true;
        }
    }
    return false;
}

bool host_lv_toggle_visible_switch(bool checked)
{
    if (_host_lv_input_is_blocked())
    {
        return false;
    }
    lv_obj_t *found = NULL;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        lv_obj_t *object = &s_objects[index];
        if (object->live && _host_lv_is_visible(object) &&
                object->kind == HOST_LV_OBJECT_SWITCH &&
                (object->state & LV_STATE_DISABLED) == 0U)
        {
            if (found != NULL)
            {
                return false;
            }
            found = object;
        }
    }
    if (found == NULL)
    {
        return false;
    }
    if (checked)
    {
        found->state |= LV_STATE_CHECKED;
    }
    else
    {
        found->state &= ~LV_STATE_CHECKED;
    }
    return _host_lv_emit(found, LV_EVENT_VALUE_CHANGED);
}

bool host_lv_click_back(void)
{
    if (_host_lv_input_is_blocked())
    {
        return false;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        lv_obj_t *object = &s_objects[index];
        if (object->live && _host_lv_is_visible(object) &&
                object->kind == HOST_LV_OBJECT_BUTTON &&
                _host_lv_has_descendant_text(object, LV_SYMBOL_LEFT) &&
                _host_lv_emit(object, LV_EVENT_CLICKED))
        {
            return true;
        }
    }
    return false;
}

bool host_lv_has_text(const char *text)
{
    if (text == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        if (s_objects[index].live && _host_lv_is_visible(&s_objects[index]) &&
                s_objects[index].kind == HOST_LV_OBJECT_LABEL &&
                strcmp(s_objects[index].text, text) == 0)
        {
            return true;
        }
    }
    return false;
}

size_t host_lv_visible_text_count(const char *text)
{
    if (text == NULL)
    {
        return 0U;
    }
    size_t count = 0U;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        if (s_objects[index].live && _host_lv_is_visible(&s_objects[index]) &&
                s_objects[index].kind == HOST_LV_OBJECT_LABEL &&
                strcmp(s_objects[index].text, text) == 0)
        {
            ++count;
        }
    }
    return count;
}

bool host_lv_text_has_font(const char *text, const lv_font_t *font)
{
    if (text == NULL || font == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        const lv_obj_t *object = &s_objects[index];
        if (object->live && _host_lv_is_visible(object) &&
                object->kind == HOST_LV_OBJECT_LABEL &&
                strcmp(object->text, text) == 0 && object->text_font == font)
        {
            return true;
        }
    }
    return false;
}

bool host_lv_transition_target_has_text(const char *text)
{
    if (text == NULL)
    {
        return false;
    }
    if (s_transition.pending && s_transition.new_screen != NULL)
    {
        return _host_lv_has_descendant_text(s_transition.new_screen, text);
    }
    for (size_t index = 0; index < HOST_LV_GENERIC_ANIM_CAPACITY; ++index)
    {
        if (s_generic_animations[index].live && s_active_screen != NULL)
        {
            return _host_lv_has_descendant_text(s_active_screen, text);
        }
    }
    return false;
}

size_t host_lv_live_object_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        count += s_objects[index].live &&
                 s_objects[index].kind != HOST_LV_OBJECT_SCREEN &&
                 !_host_lv_is_descendant(&s_objects[index], &s_top_layer) &&
                 !_host_lv_is_descendant(&s_objects[index], &s_sys_layer) ?
                 1U : 0U;
    }
    return count;
}

size_t host_lv_live_screen_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        count += s_objects[index].live &&
                 s_objects[index].kind == HOST_LV_OBJECT_SCREEN ? 1U : 0U;
    }
    return count;
}

size_t host_lv_live_timer_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < HOST_LV_TIMER_CAPACITY; ++index)
    {
        count += s_timers[index].live && !s_timers[index].paused ? 1U : 0U;
    }
    return count;
}

size_t host_lv_live_qrcode_count(void)
{
    size_t count = 0U;
    for (size_t index = 0U; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        count += s_objects[index].live &&
                 s_objects[index].kind == HOST_LV_OBJECT_QRCODE ? 1U : 0U;
    }
    return count;
}

unsigned host_lv_qrcode_update_count(void)
{
    return s_qrcode_update_count;
}

unsigned host_lv_qrcode_scrubbed_delete_count(void)
{
    return s_qrcode_scrubbed_delete_count;
}

size_t host_lv_system_object_count(void)
{
    size_t count = 0U;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        count += s_objects[index].live &&
                 _host_lv_is_descendant(&s_objects[index], &s_sys_layer) ?
                 1U : 0U;
    }
    return count;
}

static void _host_lv_snapshot_object(
    const lv_obj_t *object, host_lv_system_object_snapshot_t *snapshot)
{
    const lv_color_format_t canvas_color_format = object->draw_buf == NULL ?
        LV_COLOR_FORMAT_UNKNOWN :
        object->draw_buf->header.cf;
    *snapshot = (host_lv_system_object_snapshot_t)
    {
        .object = object,
        .visible = _host_lv_is_visible(object),
        .flags = object->flags,
        .x = object->x,
        .y = object->y,
        .width = object->width,
        .height = object->height,
        .opacity = object->opacity,
        .background_opacity = object->background_opacity,
        .text_opacity = object->text_opacity,
        .canvas = object->kind == HOST_LV_OBJECT_CANVAS,
        .canvas_color_format = canvas_color_format,
        .image_recolor = object->image_recolor,
        .image_opacity = object->image_opacity,
        .canvas_flush_count = object->draw_buf == NULL ? 0U :
                              object->draw_buf->flush_count,
                              .invalidation_count = object->invalidation_count,
                              .text = object->text,
                              .label_long_mode = object->label_long_mode,
    };
}

bool host_lv_system_object_snapshot(
    size_t index, host_lv_system_object_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return false;
    }

    size_t object_index = 0U;
    for (size_t pool_index = 0; pool_index < HOST_LV_OBJECT_CAPACITY;
            ++pool_index)
    {
        const lv_obj_t *object = &s_objects[pool_index];
        if (!object->live ||
                !_host_lv_is_descendant(object, &s_sys_layer))
        {
            continue;
        }
        if (object_index++ == index)
        {
            _host_lv_snapshot_object(object, snapshot);
            return true;
        }
    }
    return false;
}

bool host_lv_system_label_snapshot(
    const char *text, host_lv_system_object_snapshot_t *snapshot)
{
    if (text == NULL)
    {
        return false;
    }

    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        const lv_obj_t *object = &s_objects[index];
        if (object->live && object->kind == HOST_LV_OBJECT_LABEL &&
                _host_lv_is_descendant(object, &s_sys_layer) &&
                _host_lv_is_visible(object) &&
                strcmp(object->text, text) == 0)
        {
            if (snapshot != NULL)
            {
                _host_lv_snapshot_object(object, snapshot);
            }
            return true;
        }
    }
    return false;
}

bool host_lv_visible_label_snapshot(
    const char *text, const lv_font_t *font,
    host_lv_system_object_snapshot_t *snapshot)
{
    if (text == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        const lv_obj_t *object = &s_objects[index];
        if (object->live && object->kind == HOST_LV_OBJECT_LABEL &&
                _host_lv_is_visible(object) && strcmp(object->text, text) == 0 &&
                object->text_font == font)
        {
            if (snapshot != NULL)
            {
                _host_lv_snapshot_object(object, snapshot);
            }
            return true;
        }
    }
    return false;
}

bool host_lv_canvas_alpha_snapshot(const lv_obj_t *canvas, int32_t x,
                                   int32_t y, lv_opa_t *alpha)
{
    if (canvas == NULL || !canvas->live ||
            canvas->kind != HOST_LV_OBJECT_CANVAS ||
            canvas->draw_buf == NULL ||
            canvas->draw_buf->header.cf != LV_COLOR_FORMAT_A8 ||
            canvas->draw_buf->data == NULL || alpha == NULL || x < 0 || y < 0 ||
            (uint32_t)x >= canvas->draw_buf->header.w ||
            (uint32_t)y >= canvas->draw_buf->header.h)
    {
        return false;
    }

    const uint64_t offset =
        (uint64_t)(uint32_t)y * canvas->draw_buf->header.stride +
        (uint32_t)x;
    if (offset >= canvas->draw_buf->data_size)
    {
        return false;
    }
    *alpha = canvas->draw_buf->data[offset];
    return true;
}

bool host_lv_pointer_target_snapshot(
    host_lv_system_object_snapshot_t *snapshot)
{
    const lv_obj_t *target = s_pointer_indev.active_object;
    if (snapshot == NULL || target == NULL || !target->live)
    {
        return false;
    }

    _host_lv_snapshot_object(target, snapshot);
    return true;
}

bool host_lv_active_screen_snapshot(
    host_lv_system_object_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_active_screen == NULL ||
            !s_active_screen->live)
    {
        return false;
    }

    _host_lv_snapshot_object(s_active_screen, snapshot);
    return true;
}

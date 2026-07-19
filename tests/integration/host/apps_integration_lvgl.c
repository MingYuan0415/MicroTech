#include "apps_integration_runtime.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HOST_LV_OBJECT_CAPACITY 256U
#define HOST_LV_EVENT_CAPACITY  4U
#define HOST_LV_TIMER_CAPACITY  16U
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
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    lv_opa_t opacity;
    lv_opa_t background_opacity;
    lv_obj_flag_t flags;
    lv_event_dsc_t bindings[HOST_LV_EVENT_CAPACITY];
    size_t binding_count;
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

const lv_font_t host_lv_default_font = {.marker = 1};

static lv_obj_t s_initial_screen;
static lv_obj_t s_top_layer;
static lv_obj_t *s_active_screen;
static lv_display_t s_display = {.marker = 1U};
static lv_obj_t s_objects[HOST_LV_OBJECT_CAPACITY];
static lv_timer_t s_timers[HOST_LV_TIMER_CAPACITY];
static host_lv_transition_t s_transition;

static void _host_lv_set_x_animation(void *variable, int32_t value);
static void _host_lv_set_y_animation(void *variable, int32_t value);
static void _host_lv_set_opacity_animation(void *variable, int32_t value);

static lv_obj_t *_host_lv_allocate_object(host_lv_object_kind_t kind,
        lv_obj_t *parent)
{
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
            if (kind == HOST_LV_OBJECT_BUTTON ||
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
    if (object == s_active_screen || object == &s_top_layer)
    {
        return true;
    }
    return _host_lv_is_descendant(object, s_active_screen) ||
           _host_lv_is_descendant(object, &s_top_layer);
}

static bool _host_lv_input_is_blocked(void)
{
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        const lv_obj_t *object = &s_objects[index];
        if (object->live &&
                (object->flags & LV_OBJ_FLAG_CLICKABLE) != 0U &&
                (object->flags & LV_OBJ_FLAG_HIDDEN) == 0U &&
                _host_lv_is_descendant(object, &s_top_layer))
        {
            return true;
        }
    }
    return false;
}

static bool _host_lv_emit(lv_obj_t *object, lv_event_code_t code)
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
            };
            binding.callback(&event);
            emitted = true;
        }
    }
    return emitted;
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
    (void)_host_lv_emit(screen, LV_EVENT_SCREEN_LOAD_START);
    s_active_screen = screen;
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
    memset(&s_initial_screen, 0, sizeof(s_initial_screen));
    s_initial_screen.live = true;
    s_initial_screen.kind = HOST_LV_OBJECT_SCREEN;
    s_initial_screen.width = HOST_LV_HOR_RES;
    s_initial_screen.height = HOST_LV_VER_RES;
    s_initial_screen.opacity = LV_OPA_COVER;
    memset(&s_top_layer, 0, sizeof(s_top_layer));
    s_top_layer.live = true;
    s_top_layer.kind = HOST_LV_OBJECT_LAYER;
    s_top_layer.width = HOST_LV_HOR_RES;
    s_top_layer.height = HOST_LV_VER_RES;
    s_top_layer.opacity = LV_OPA_COVER;
    s_active_screen = &s_initial_screen;
    memset(s_objects, 0, sizeof(s_objects));
    memset(s_timers, 0, sizeof(s_timers));
    memset(&s_transition, 0, sizeof(s_transition));
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

void lv_screen_load(lv_obj_t *screen)
{
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

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
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
    return _host_lv_allocate_object(HOST_LV_OBJECT_LABEL, parent);
}

lv_obj_t *lv_slider_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_SLIDER, parent);
}

void lv_obj_delete(lv_obj_t *object)
{
    if (object == NULL || object == &s_initial_screen ||
            object == &s_top_layer || !object->live)
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
    _host_lv_delete_object(object);
}

void lv_obj_clean(lv_obj_t *object)
{
    if (object == NULL || !object->live)
    {
        return;
    }
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

bool lv_obj_is_valid(const lv_obj_t *object)
{
    if (object == &s_initial_screen || object == &s_top_layer)
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

void lv_obj_move_to_index(lv_obj_t *object, int32_t index)
{
    (void)object;
    (void)index;
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
        object->width = width;
        object->height = height;
    }
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

void lv_obj_remove_style_all(lv_obj_t *object)
{
    if (object != NULL && object->live)
    {
        object->opacity = LV_OPA_COVER;
        object->background_opacity = LV_OPA_TRANSP;
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
    return deleted;
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

void lv_anim_refr_now(void)
{
    if (!s_transition.pending)
    {
        return;
    }
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
                              ((new_animation->end_value -
                                new_animation->start_value) * time) / duration;
        new_animation->current_value = value;
        new_animation->exec_cb(new_animation->var, value);
    }
    if (new_animation->act_time >= new_animation->duration)
    {
        _host_lv_complete_transition();
    }
    _host_lv_run_ready_timers();
}

void app_manager_host_ui_step(void)
{
    if (!s_transition.pending || !s_transition.new_animation_live)
    {
        return;
    }

    lv_anim_t *animation = &s_transition.new_animation;
    animation->act_time += HOST_LV_ANIM_STEP_MS;
    if (s_transition.old_animation_live)
    {
        s_transition.old_animation.act_time += HOST_LV_ANIM_STEP_MS;
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
                _host_lv_has_descendant_text(object, title) &&
                _host_lv_emit(object, LV_EVENT_CLICKED))
        {
            return true;
        }
    }
    return false;
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

bool host_lv_transition_target_has_text(const char *text)
{
    return text != NULL && s_transition.pending &&
           s_transition.new_screen != NULL &&
           _host_lv_has_descendant_text(s_transition.new_screen, text);
}

size_t host_lv_live_object_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        count += s_objects[index].live &&
                 s_objects[index].kind != HOST_LV_OBJECT_SCREEN &&
                 !_host_lv_is_descendant(&s_objects[index], &s_top_layer) ?
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

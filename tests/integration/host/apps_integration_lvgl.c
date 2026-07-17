#include "apps_integration_runtime.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HOST_LV_OBJECT_CAPACITY 256U
#define HOST_LV_EVENT_CAPACITY  4U
#define HOST_LV_TIMER_CAPACITY  16U
#define HOST_LV_TEXT_BYTES      160U

typedef enum
{
    HOST_LV_OBJECT_GENERIC = 0,
    HOST_LV_OBJECT_BUTTON,
    HOST_LV_OBJECT_LABEL,
    HOST_LV_OBJECT_SLIDER,
} host_lv_object_kind_t;

typedef struct host_lv_event_binding
{
    lv_event_cb_t callback;
    lv_event_code_t code;
    void *user_data;
} host_lv_event_binding_t;

struct lv_obj_t
{
    bool live;
    host_lv_object_kind_t kind;
    lv_obj_t *parent;
    char text[HOST_LV_TEXT_BYTES];
    int32_t slider_minimum;
    int32_t slider_maximum;
    int32_t slider_value;
    host_lv_event_binding_t bindings[HOST_LV_EVENT_CAPACITY];
    size_t binding_count;
};

struct lv_timer_t
{
    bool live;
    lv_timer_cb_t callback;
    uint32_t period;
    void *user_data;
};

struct lv_event_t
{
    lv_event_code_t code;
    void *user_data;
};

const lv_font_t host_lv_default_font = {.marker = 1};

static lv_obj_t s_screen;
static lv_obj_t s_objects[HOST_LV_OBJECT_CAPACITY];
static lv_timer_t s_timers[HOST_LV_TIMER_CAPACITY];

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

static bool _host_lv_emit(lv_obj_t *object, lv_event_code_t code)
{
    if (object == NULL || !object->live)
    {
        return false;
    }
    for (size_t index = 0; index < object->binding_count; ++index)
    {
        const host_lv_event_binding_t binding = object->bindings[index];
        if (binding.code == code && binding.callback != NULL)
        {
            lv_event_t event =
            {
                .code = code,
                .user_data = binding.user_data,
            };
            binding.callback(&event);
            return true;
        }
    }
    return false;
}

void host_lv_reset(void)
{
    memset(&s_screen, 0, sizeof(s_screen));
    s_screen.live = true;
    memset(s_objects, 0, sizeof(s_objects));
    memset(s_timers, 0, sizeof(s_timers));
}

lv_obj_t *lv_screen_active(void)
{
    return &s_screen;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    return _host_lv_allocate_object(HOST_LV_OBJECT_GENERIC, parent);
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
    if (object == NULL || object == &s_screen || !object->live)
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
    object->live = false;
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

void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback,
                         lv_event_code_t code, void *user_data)
{
    if (object == NULL || !object->live || callback == NULL ||
            object->binding_count == HOST_LV_EVENT_CAPACITY)
    {
        return;
    }
    object->bindings[object->binding_count++] = (host_lv_event_binding_t)
    {
        .callback = callback,
        .code = code,
        .user_data = user_data,
    };
}

lv_event_code_t lv_event_get_code(lv_event_t *event)
{
    return event == NULL ? 0 : event->code;
}

void *lv_event_get_user_data(lv_event_t *event)
{
    return event == NULL ? NULL : event->user_data;
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

void *lv_timer_get_user_data(lv_timer_t *timer)
{
    return timer == NULL ? NULL : timer->user_data;
}

bool host_lv_click_action(const char *title)
{
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        lv_obj_t *object = &s_objects[index];
        if (object->live && object->kind == HOST_LV_OBJECT_BUTTON &&
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
    for (size_t index = 0; index < HOST_LV_OBJECT_CAPACITY; ++index)
    {
        lv_obj_t *object = &s_objects[index];
        if (object->live && object->kind == HOST_LV_OBJECT_BUTTON &&
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
        if (s_objects[index].live &&
                s_objects[index].kind == HOST_LV_OBJECT_LABEL &&
                strcmp(s_objects[index].text, text) == 0)
        {
            return true;
        }
    }
    return false;
}

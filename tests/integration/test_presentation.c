#include "apps_integration_runtime.h"
#include "app_manager_presentation.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static size_t s_completion_count;
static size_t s_click_count;

static void _transition_completed(void *context)
{
    size_t *count = context;
    (*count)++;
}

static void _button_clicked(lv_event_t *event)
{
    (void)event;
    s_click_count++;
}

static void _complete_normally(lv_obj_t *target)
{
    lv_anim_t *animation = lv_anim_get(target, NULL);
    assert(animation != NULL);
    animation->act_time = animation->duration;
    lv_anim_refr_now();
}

static void _test_effect_matrix(void)
{
    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);

    lv_obj_t *screens[2] =
    {
        app_manager_presentation_create_page_screen(),
        app_manager_presentation_create_page_screen(),
    };
    assert(screens[0] != NULL && screens[1] != NULL);
    assert(app_manager_presentation_load_immediate(screens[0]) == ESP_OK);

    size_t active_index = 0U;
    for (app_manager_transition_effect_t effect = APP_MANAGER_TRANSITION_NONE;
            effect < APP_MANAGER_TRANSITION_END; effect++)
    {
        const size_t target_index = active_index ^ 1U;
        s_completion_count = 0U;
        assert(app_manager_presentation_start(
                   screens[active_index], screens[target_index], effect, 220U,
                   _transition_completed, &s_completion_count) == ESP_OK);
        if (effect == APP_MANAGER_TRANSITION_NONE)
        {
            assert(!app_manager_presentation_is_running());
            assert(s_completion_count == 1U);
        }
        else
        {
            assert(app_manager_presentation_is_running());
            assert(s_completion_count == 0U);
            _complete_normally(screens[target_index]);
            assert(!app_manager_presentation_is_running());
            assert(s_completion_count == 1U);
            app_manager_presentation_finish_now();
            assert(s_completion_count == 1U);
        }
        assert(lv_screen_active() == screens[target_index]);
        active_index = target_index;
    }

    assert(app_manager_presentation_start(
               screens[active_index], screens[active_index ^ 1U],
               APP_MANAGER_TRANSITION_DEFAULT, 220U,
               _transition_completed, &s_completion_count) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_presentation_start(
               screens[active_index], screens[active_index ^ 1U],
               APP_MANAGER_TRANSITION_FADE,
               APP_MANAGER_TRANSITION_MAX_DURATION_MS + 1U,
               _transition_completed, &s_completion_count) ==
           ESP_ERR_INVALID_ARG);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(screens[0]) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(screens[1]) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
}

static void _test_barrier_and_fast_forward(void)
{
    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);
    assert(host_lv_system_object_count() == 1U);
    host_lv_system_object_snapshot_t blocker;
    assert(host_lv_system_object_snapshot(0U, &blocker));
    assert(!blocker.visible);
    assert((blocker.flags & LV_OBJ_FLAG_CLICKABLE) != 0U);
    assert(blocker.width == 368 && blocker.height == 448);
    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);

    lv_obj_t *button = lv_button_create(source);
    lv_obj_t *label = lv_label_create(button);
    assert(button != NULL && label != NULL);
    lv_label_set_text(label, "action");
    assert(lv_obj_add_event_cb(button, _button_clicked, LV_EVENT_CLICKED,
                               NULL) != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);
    s_click_count = 0U;
    assert(host_lv_click_action("action"));
    assert(s_click_count == 1U);

    lv_obj_t *system_underlay = lv_obj_create(lv_layer_sys());
    assert(system_underlay != NULL);
    lv_obj_set_size(system_underlay, LV_PCT(100), LV_PCT(100));

    s_completion_count = 0U;
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_PUSH_LEFT, 220U,
               _transition_completed, &s_completion_count) == ESP_OK);
    assert(host_lv_system_object_snapshot(0U, &blocker));
    assert(blocker.visible);
    host_lv_system_object_snapshot_t pointer_target;
    assert(host_lv_touch_press(184, 224));
    assert(host_lv_pointer_target_snapshot(&pointer_target));
    assert(pointer_target.object == blocker.object);
    assert(host_lv_touch_move(185, 225));
    assert(host_lv_pointer_target_snapshot(&pointer_target));
    assert(pointer_target.object == blocker.object);
    assert(host_lv_touch_release(185, 225));
    assert(!host_lv_pointer_target_snapshot(&pointer_target));
    assert(!host_lv_click_action("action"));
    app_manager_presentation_finish_now();
    assert(host_lv_system_object_snapshot(0U, &blocker));
    assert(!blocker.visible);
    assert(s_completion_count == 1U);
    assert(lv_screen_active() == target);
    app_manager_presentation_finish_now();
    assert(s_completion_count == 1U);
    lv_obj_delete(system_underlay);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(target) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(host_lv_system_object_count() == 0U);
}

int main(void)
{
    _test_effect_matrix();
    _test_barrier_and_fast_forward();
    return 0;
}

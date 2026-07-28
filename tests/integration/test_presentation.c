#include "apps_integration_runtime.h"
#include "app_manager_display_diagnostics_priv.h"
#include "app_manager_presentation.h"
#include "esp_heap_caps.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static size_t s_completion_count;
static size_t s_click_count;

typedef struct transition_completion_context
{
    size_t *count;
    lv_obj_t *target;
    size_t target_child_count;
} transition_completion_context_t;

static transition_completion_context_t s_completion_context;

typedef enum presentation_diagnostics_event
{
    PRESENTATION_DIAGNOSTICS_STARTED = 0,
    PRESENTATION_DIAGNOSTICS_COMPLETED,
    PRESENTATION_DIAGNOSTICS_CANCELLED,
} presentation_diagnostics_event_t;

static presentation_diagnostics_event_t s_diagnostics_events[4];
static size_t s_diagnostics_event_count;
static app_manager_transition_effect_t s_diagnostics_effect;
static size_t s_snapshot_prepare_start_count;
static size_t s_snapshot_prepare_finish_count;
static size_t s_snapshot_fallback_count;
static app_manager_transition_effect_t s_snapshot_prepare_effect;

#if CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION
    #define TEST_SNAPSHOT_ANIMATION_ENABLED 1
    #define TEST_SNAPSHOT_BUFFER_BYTES (368U * 448U * 2U)
#else
    #define TEST_SNAPSHOT_ANIMATION_ENABLED 0
#endif

static void _diagnostics_reset(void)
{
    s_diagnostics_event_count = 0U;
    s_diagnostics_effect = APP_MANAGER_TRANSITION_NONE;
    s_snapshot_prepare_start_count = 0U;
    s_snapshot_prepare_finish_count = 0U;
    s_snapshot_fallback_count = 0U;
    s_snapshot_prepare_effect = APP_MANAGER_TRANSITION_NONE;
}

static void _diagnostics_record(presentation_diagnostics_event_t event)
{
    assert(s_diagnostics_event_count <
           sizeof(s_diagnostics_events) / sizeof(s_diagnostics_events[0]));
    s_diagnostics_events[s_diagnostics_event_count++] = event;
}

void app_manager_display_diagnostics_transition_started(
    app_manager_transition_effect_t effect)
{
    s_diagnostics_effect = effect;
    _diagnostics_record(PRESENTATION_DIAGNOSTICS_STARTED);
}

void app_manager_display_diagnostics_snapshot_prepare_started(
    app_manager_transition_effect_t effect)
{
    s_snapshot_prepare_effect = effect;
    s_snapshot_prepare_start_count++;
}

void app_manager_display_diagnostics_snapshot_prepare_finished(bool fallback)
{
    s_snapshot_prepare_finish_count++;
    s_snapshot_fallback_count += fallback ? 1U : 0U;
}

void app_manager_display_diagnostics_transition_completed(void)
{
    _diagnostics_record(PRESENTATION_DIAGNOSTICS_COMPLETED);
}

void app_manager_display_diagnostics_transition_cancelled(void)
{
    _diagnostics_record(PRESENTATION_DIAGNOSTICS_CANCELLED);
}

static void _transition_completed(void *context)
{
    transition_completion_context_t *completion = context;

    assert(completion != NULL);
    assert(completion->count != NULL);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(completion->target != NULL);
    assert(lv_obj_is_valid(completion->target));
    assert(host_lv_object_child_count(completion->target) ==
           completion->target_child_count);
    assert(host_lv_generic_animation_count() == 0U);
#endif
    (*completion->count)++;
}

static void _prepare_completion(lv_obj_t *target)
{
    s_completion_context.count = &s_completion_count;
    s_completion_context.target = target;
    s_completion_context.target_child_count =
        host_lv_object_child_count(target);
}

static void _button_clicked(lv_event_t *event)
{
    (void)event;
    s_click_count++;
}

#if TEST_SNAPSHOT_ANIMATION_ENABLED
static void _delete_screen_on_load(lv_event_t *event)
{
    lv_obj_t *screen = lv_event_get_target_obj(event);

    assert(screen != NULL);
    lv_obj_delete(screen);
}
#endif

static void _complete_normally(lv_obj_t *target)
{
    lv_anim_t *animation = lv_anim_get(target, NULL);
    if (animation != NULL)
    {
        animation->act_time = animation->duration;
        lv_anim_refr_now();
        return;
    }

    assert(host_lv_generic_animation_count() == 1U);
    host_lv_complete_generic_animations();
}

#if TEST_SNAPSHOT_ANIMATION_ENABLED
typedef struct snapshot_image_state
{
    host_lv_object_snapshot_t from;
    host_lv_object_snapshot_t to;
} snapshot_image_state_t;

static int32_t _snapshot_lerp(int32_t start, int32_t end,
                              int32_t progress)
{
    return start + ((end - start) * progress) / 1000;
}

static bool _snapshot_effect_is_reveal(app_manager_transition_effect_t effect)
{
    return effect >= APP_MANAGER_TRANSITION_REVEAL_LEFT &&
           effect <= APP_MANAGER_TRANSITION_REVEAL_DOWN;
}

static void _snapshot_expected_state(app_manager_transition_effect_t effect,
                                     int32_t progress,
                                     snapshot_image_state_t *expected)
{
    assert(expected != NULL);
    *expected = (snapshot_image_state_t)
    {
        .from =
        {
            .x = 0,
            .y = 0,
            .opacity = LV_OPA_COVER,
        },
        .to =
        {
            .x = 0,
            .y = 0,
            .opacity = LV_OPA_COVER,
        },
    };

    switch (effect)
    {
    case APP_MANAGER_TRANSITION_FADE:
        expected->to.opacity = (lv_opa_t)_snapshot_lerp(
                                   LV_OPA_TRANSP, LV_OPA_COVER, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_LEFT:
        expected->to.x = _snapshot_lerp(368, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_RIGHT:
        expected->to.x = _snapshot_lerp(-368, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_UP:
        expected->to.y = _snapshot_lerp(448, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_DOWN:
        expected->to.y = _snapshot_lerp(-448, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_LEFT:
        expected->from.x = _snapshot_lerp(0, -368, progress);
        expected->to.x = _snapshot_lerp(368, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_RIGHT:
        expected->from.x = _snapshot_lerp(0, 368, progress);
        expected->to.x = _snapshot_lerp(-368, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_UP:
        expected->from.y = _snapshot_lerp(0, -448, progress);
        expected->to.y = _snapshot_lerp(448, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_DOWN:
        expected->from.y = _snapshot_lerp(0, 448, progress);
        expected->to.y = _snapshot_lerp(-448, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_LEFT:
        expected->from.x = _snapshot_lerp(0, -368, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_RIGHT:
        expected->from.x = _snapshot_lerp(0, 368, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_UP:
        expected->from.y = _snapshot_lerp(0, -448, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_DOWN:
        expected->from.y = _snapshot_lerp(0, 448, progress);
        break;
    case APP_MANAGER_TRANSITION_NONE:
    case APP_MANAGER_TRANSITION_DEFAULT:
    case APP_MANAGER_TRANSITION_END:
    default:
        assert(false);
        break;
    }
}

static snapshot_image_state_t _snapshot_overlay_images(
    lv_obj_t *target, app_manager_transition_effect_t effect)
{
    host_lv_object_snapshot_t overlay;
    host_lv_object_snapshot_t first_image;
    host_lv_object_snapshot_t second_image;

    const size_t target_child_count = host_lv_object_child_count(target);
    assert(target_child_count >= 1U);
    assert(host_lv_object_child_snapshot(target, target_child_count - 1U,
                                         &overlay));
    assert(!overlay.image);
    assert(overlay.parent == target);
    assert(overlay.x == 0 && overlay.y == 0);
    assert(overlay.width == 368 && overlay.height == 448);
    assert(overlay.opacity == LV_OPA_COVER);
    assert(overlay.background_opacity == LV_OPA_COVER);
    assert((overlay.flags & (LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE)) ==
           0U);
    assert((overlay.flags & LV_OBJ_FLAG_IGNORE_LAYOUT) != 0U);
    assert(host_lv_object_child_count(overlay.object) == 2U);
    assert(host_lv_object_child_snapshot(overlay.object, 0U, &first_image));
    assert(host_lv_object_child_snapshot(overlay.object, 1U, &second_image));
    assert(first_image.image && second_image.image);
    assert(first_image.has_draw_buf_source && second_image.has_draw_buf_source);
    assert(first_image.width == 368 && first_image.height == 448);
    assert(second_image.width == 368 && second_image.height == 448);

    return (snapshot_image_state_t)
    {
        .from = _snapshot_effect_is_reveal(effect) ?
                second_image : first_image,
                .to = _snapshot_effect_is_reveal(effect) ?
                      first_image : second_image,
    };
}

static void _assert_snapshot_images(lv_obj_t *target,
                                    app_manager_transition_effect_t effect,
                                    int32_t progress)
{
    snapshot_image_state_t actual = _snapshot_overlay_images(target, effect);
    snapshot_image_state_t expected;
    _snapshot_expected_state(effect, progress, &expected);
    assert(actual.from.x == expected.from.x);
    assert(actual.from.y == expected.from.y);
    assert(actual.from.opacity == expected.from.opacity);
    assert(actual.to.x == expected.to.x);
    assert(actual.to.y == expected.to.y);
    assert(actual.to.opacity == expected.to.opacity);
}

static void _assert_snapshot_captures(lv_obj_t *source, lv_obj_t *target,
                                      size_t capture_base)
{
    const lv_obj_t *captured = NULL;
    bool input_blocked = false;
    bool object_active = false;

    assert(host_lv_snapshot_capture_count() == capture_base + 2U);
    assert(host_lv_snapshot_capture_at(capture_base, &captured, &input_blocked,
                                       &object_active));
    assert(captured == source);
    assert(input_blocked && object_active);
    assert(host_lv_snapshot_capture_at(capture_base + 1U, &captured,
                                       &input_blocked, &object_active));
    assert(captured == target);
    assert(input_blocked && !object_active);
}
#endif

static void _test_effect_matrix(void)
{
    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(host_lv_snapshot_live_allocation_count() == 2U);
    assert(host_lv_snapshot_live_allocation_bytes() ==
           2U * TEST_SNAPSHOT_BUFFER_BYTES);
    assert(host_lv_snapshot_allocation_call_count() == 2U);
    for (size_t index = 0U; index < 2U; ++index)
    {
        host_lv_snapshot_allocation_call_t call;
        assert(host_lv_snapshot_allocation_call_at(index, &call));
        assert(call.alignment == LV_DRAW_BUF_ALIGN);
        assert(call.count == 1U);
        assert(call.size == TEST_SNAPSHOT_BUFFER_BYTES);
        assert(call.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        assert((call.caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) == 0U);
        assert(call.succeeded);
    }
#endif

    lv_obj_t *screens[2] =
    {
        app_manager_presentation_create_page_screen(),
        app_manager_presentation_create_page_screen(),
    };
    assert(screens[0] != NULL && screens[1] != NULL);
    for (size_t index = 0U; index < 2U; ++index)
    {
        lv_obj_t *root = lv_obj_create(screens[index]);
        assert(root != NULL);
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    }
    assert(app_manager_presentation_load_immediate(screens[0]) == ESP_OK);

    size_t active_index = 0U;
    for (app_manager_transition_effect_t effect = APP_MANAGER_TRANSITION_NONE;
            effect < APP_MANAGER_TRANSITION_END; effect++)
    {
        const size_t target_index = active_index ^ 1U;
#if TEST_SNAPSHOT_ANIMATION_ENABLED
        const size_t capture_base = host_lv_snapshot_capture_count();
        const size_t live_object_count = host_lv_live_object_count();
        const size_t animation_start_base =
            host_lv_generic_animation_start_count();
        const size_t animation_completion_base =
            host_lv_generic_animation_completion_count();
#elif CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED
        const size_t capture_base = host_lv_snapshot_capture_count();
#endif
        s_completion_count = 0U;
        _diagnostics_reset();
        _prepare_completion(screens[target_index]);
        assert(app_manager_presentation_start(
                   screens[active_index], screens[target_index], effect, 220U,
                   _transition_completed, &s_completion_context) == ESP_OK);
        if (effect == APP_MANAGER_TRANSITION_NONE)
        {
            assert(!app_manager_presentation_is_running());
            assert(s_completion_count == 1U);
            assert(s_diagnostics_event_count == 0U);
        }
        else
        {
            assert(app_manager_presentation_is_running());
            assert(s_completion_count == 0U);
            assert(s_diagnostics_event_count == 1U);
            assert(s_diagnostics_events[0] ==
                   PRESENTATION_DIAGNOSTICS_STARTED);
            assert(s_diagnostics_effect == effect);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
            assert(s_snapshot_prepare_start_count == 1U);
            assert(s_snapshot_prepare_finish_count == 1U);
            assert(s_snapshot_fallback_count == 0U);
            assert(s_snapshot_prepare_effect == effect);
            _assert_snapshot_captures(screens[active_index],
                                      screens[target_index], capture_base);
            assert(lv_screen_active() == screens[target_index]);
            assert(host_lv_generic_animation_count() == 1U);
            assert(host_lv_generic_animation_start_count() ==
                   animation_start_base + 1U);
            _assert_snapshot_images(screens[target_index], effect, 0);
            host_lv_advance_generic_animations(110U);
            assert(app_manager_presentation_is_running());
            _assert_snapshot_images(screens[target_index], effect, 500);
#elif CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED
            assert(host_lv_snapshot_capture_count() == capture_base);
            assert(host_lv_generic_animation_count() == 0U);
#endif
            _complete_normally(screens[target_index]);
            assert(!app_manager_presentation_is_running());
            assert(s_completion_count == 1U);
            assert(s_diagnostics_event_count == 2U);
            assert(s_diagnostics_events[1] ==
                   PRESENTATION_DIAGNOSTICS_COMPLETED);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
            int32_t completion_value = -1;
            assert(host_lv_generic_animation_completion_count() ==
                   animation_completion_base + 1U);
            assert(host_lv_generic_animation_completion_value_at(
                       animation_completion_base, &completion_value));
            assert(completion_value == 1000);
            assert(host_lv_generic_animation_count() == 0U);
            assert(host_lv_object_child_count(screens[target_index]) == 1U);
            assert(host_lv_live_object_count() == live_object_count);
            assert(host_lv_snapshot_live_allocation_count() == 2U);
            assert(host_lv_snapshot_live_allocation_bytes() ==
                   2U * TEST_SNAPSHOT_BUFFER_BYTES);
#endif
            app_manager_presentation_finish_now();
            assert(s_completion_count == 1U);
            assert(s_diagnostics_event_count == 2U);
        }
#if TEST_SNAPSHOT_ANIMATION_ENABLED
        if (effect == APP_MANAGER_TRANSITION_NONE)
        {
            assert(host_lv_snapshot_capture_count() == capture_base);
            assert(host_lv_generic_animation_start_count() ==
                   animation_start_base);
            assert(host_lv_generic_animation_completion_count() ==
                   animation_completion_base);
            assert(s_snapshot_prepare_start_count == 0U);
            assert(s_snapshot_prepare_finish_count == 0U);
        }
#endif
        assert(lv_screen_active() == screens[target_index]);
        active_index = target_index;
    }

    _prepare_completion(screens[active_index ^ 1U]);
    assert(app_manager_presentation_start(
               screens[active_index], screens[active_index ^ 1U],
               APP_MANAGER_TRANSITION_DEFAULT, 220U,
               _transition_completed, &s_completion_context) ==
           ESP_ERR_INVALID_ARG);
    assert(app_manager_presentation_start(
               screens[active_index], screens[active_index ^ 1U],
               APP_MANAGER_TRANSITION_FADE,
               APP_MANAGER_TRANSITION_MAX_DURATION_MS + 1U,
               _transition_completed, &s_completion_context) ==
           ESP_ERR_INVALID_ARG);
    assert(s_diagnostics_event_count == 2U);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(screens[0]) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(screens[1]) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(host_lv_snapshot_live_allocation_count() == 0U);
    assert(host_lv_snapshot_live_allocation_bytes() == 0U);
#endif
}

static void _test_no_animation_does_not_report_diagnostics(void)
{
    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(host_lv_snapshot_live_allocation_count() == 2U);
#endif

    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);

    s_completion_count = 0U;
    _diagnostics_reset();
    _prepare_completion(source);
    assert(app_manager_presentation_start(
               source, source, APP_MANAGER_TRANSITION_FADE, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(s_completion_count == 1U);
    assert(s_diagnostics_event_count == 0U);

    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_FADE, 0U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(s_completion_count == 2U);
    assert(s_diagnostics_event_count == 0U);

    _prepare_completion(source);
    assert(app_manager_presentation_start(
               target, source, APP_MANAGER_TRANSITION_NONE, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(s_completion_count == 3U);
    assert(s_diagnostics_event_count == 0U);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(target) == ESP_OK);
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

    lv_obj_t *source_root = lv_obj_create(source);
    assert(source_root != NULL);
    lv_obj_remove_style_all(source_root);
    lv_obj_set_size(source_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(source_root, LV_OPA_COVER, 0);
    lv_obj_t *button = lv_button_create(source_root);
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
    _diagnostics_reset();
    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_PUSH_LEFT, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(s_diagnostics_event_count == 1U);
    assert(s_diagnostics_events[0] == PRESENTATION_DIAGNOSTICS_STARTED);
    assert(s_diagnostics_effect == APP_MANAGER_TRANSITION_PUSH_LEFT);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(host_lv_generic_animation_count() == 1U);
    _assert_snapshot_images(target, APP_MANAGER_TRANSITION_PUSH_LEFT, 0);
#endif
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
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(host_lv_generic_animation_count() == 0U);
    assert(host_lv_object_child_count(target) == 0U);
#endif
    assert(host_lv_system_object_snapshot(0U, &blocker));
    assert(!blocker.visible);
    assert(s_completion_count == 1U);
    assert(s_diagnostics_event_count == 2U);
    assert(s_diagnostics_events[1] == PRESENTATION_DIAGNOSTICS_COMPLETED);
    assert(lv_screen_active() == target);
    app_manager_presentation_finish_now();
    assert(s_completion_count == 1U);
    assert(s_diagnostics_event_count == 2U);
    lv_obj_delete(system_underlay);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(target) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(host_lv_system_object_count() == 0U);
}

#if TEST_SNAPSHOT_ANIMATION_ENABLED
typedef enum snapshot_fallback_case
{
    SNAPSHOT_FALLBACK_ALLOCATION = 0,
    SNAPSHOT_FALLBACK_PARTIAL_ALLOCATION,
    SNAPSHOT_FALLBACK_DRAW_BUFFER,
    SNAPSHOT_FALLBACK_SOURCE_CAPTURE,
    SNAPSHOT_FALLBACK_TARGET_CAPTURE,
    SNAPSHOT_FALLBACK_TRANSPARENT_SOURCE,
    SNAPSHOT_FALLBACK_TRANSPARENT_TARGET,
    SNAPSHOT_FALLBACK_VISIBLE_OVERFLOW,
    SNAPSHOT_FALLBACK_OVERLAY,
    SNAPSHOT_FALLBACK_IMAGE,
    SNAPSHOT_FALLBACK_ANIMATION,
    SNAPSHOT_FALLBACK_TARGET_LOAD,
} snapshot_fallback_case_t;

static void _test_snapshot_fallback_case(snapshot_fallback_case_t fallback_case)
{
    size_t expected_captures = 2U;

    host_lv_reset();
    if (fallback_case == SNAPSHOT_FALLBACK_ALLOCATION)
    {
        host_lv_snapshot_fail_next_allocations(1U);
        expected_captures = 0U;
    }
    else if (fallback_case == SNAPSHOT_FALLBACK_PARTIAL_ALLOCATION)
    {
        host_lv_snapshot_fail_allocation_after(1U);
        expected_captures = 0U;
    }
    else if (fallback_case == SNAPSHOT_FALLBACK_DRAW_BUFFER)
    {
        host_lv_snapshot_fail_next_draw_buffer_initializations(1U);
        expected_captures = 0U;
    }
    assert(app_manager_presentation_init() == ESP_OK);
    const bool initialization_fallback =
        fallback_case <= SNAPSHOT_FALLBACK_DRAW_BUFFER;
    assert(host_lv_snapshot_live_allocation_count() ==
           (initialization_fallback ? 0U : 2U));
    assert(host_lv_snapshot_live_allocation_bytes() ==
           (initialization_fallback ? 0U :
            2U * TEST_SNAPSHOT_BUFFER_BYTES));

    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);

    switch (fallback_case)
    {
    case SNAPSHOT_FALLBACK_ALLOCATION:
    case SNAPSHOT_FALLBACK_PARTIAL_ALLOCATION:
    case SNAPSHOT_FALLBACK_DRAW_BUFFER:
        break;
    case SNAPSHOT_FALLBACK_SOURCE_CAPTURE:
        host_lv_snapshot_fail_next_captures(1U);
        expected_captures = 1U;
        break;
    case SNAPSHOT_FALLBACK_TARGET_CAPTURE:
        host_lv_snapshot_fail_capture_after(1U);
        break;
    case SNAPSHOT_FALLBACK_TRANSPARENT_SOURCE:
    case SNAPSHOT_FALLBACK_TRANSPARENT_TARGET:
    case SNAPSHOT_FALLBACK_VISIBLE_OVERFLOW:
    {
        lv_obj_t *root = lv_obj_create(
                             fallback_case == SNAPSHOT_FALLBACK_TRANSPARENT_SOURCE ?
                             source : target);
        assert(root != NULL);
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
        if (fallback_case == SNAPSHOT_FALLBACK_VISIBLE_OVERFLOW)
        {
            lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
            lv_obj_add_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        }
        expected_captures = 0U;
        break;
    }
    case SNAPSHOT_FALLBACK_OVERLAY:
        host_lv_snapshot_fail_next_object_creations(1U);
        break;
    case SNAPSHOT_FALLBACK_IMAGE:
        host_lv_snapshot_fail_object_creation_after(1U);
        break;
    case SNAPSHOT_FALLBACK_ANIMATION:
        host_lv_snapshot_fail_next_animation_starts(1U);
        break;
    case SNAPSHOT_FALLBACK_TARGET_LOAD:
        host_lv_snapshot_fail_next_screen_loads(1U);
        break;
    default:
        assert(false);
        break;
    }

    const size_t source_children = host_lv_object_child_count(source);
    const size_t target_children = host_lv_object_child_count(target);

    s_completion_count = 0U;
    _diagnostics_reset();
    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_FADE, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(app_manager_presentation_is_running());
    assert(s_completion_count == 0U);
    assert(host_lv_snapshot_capture_count() == expected_captures);
    assert(host_lv_generic_animation_count() == 0U);
    assert(host_lv_object_child_count(source) == source_children);
    assert(host_lv_object_child_count(target) == target_children);
    assert(s_diagnostics_event_count == 1U);
    assert(s_diagnostics_events[0] == PRESENTATION_DIAGNOSTICS_STARTED);
    assert(s_snapshot_prepare_start_count == 1U);
    assert(s_snapshot_prepare_finish_count == 1U);
    assert(s_snapshot_fallback_count == 1U);

    _complete_normally(target);
    assert(!app_manager_presentation_is_running());
    assert(s_completion_count == 1U);
    assert(s_diagnostics_event_count == 2U);
    assert(s_diagnostics_events[1] == PRESENTATION_DIAGNOSTICS_COMPLETED);
    assert(lv_screen_active() == target);
    assert(host_lv_object_child_count(source) == source_children);
    assert(host_lv_object_child_count(target) == target_children);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(target) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(host_lv_snapshot_live_allocation_count() == 0U);
    assert(host_lv_snapshot_live_allocation_bytes() == 0U);
}

static void _test_snapshot_fallbacks(void)
{
    for (int fallback_case = SNAPSHOT_FALLBACK_ALLOCATION;
            fallback_case <= SNAPSHOT_FALLBACK_TARGET_LOAD; fallback_case++)
    {
        _test_snapshot_fallback_case((snapshot_fallback_case_t)fallback_case);
    }
}

static void _test_snapshot_and_native_animation_start_failure(void)
{
    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);

    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);

    host_lv_snapshot_fail_next_animation_starts(1U);
    host_lv_fail_next_screen_animation_starts(1U);
    s_completion_count = 0U;
    _diagnostics_reset();
    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_FADE, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);

    assert(!app_manager_presentation_is_running());
    assert(s_completion_count == 1U);
    assert(s_diagnostics_event_count == 0U);
    assert(s_snapshot_prepare_start_count == 1U);
    assert(s_snapshot_prepare_finish_count == 1U);
    assert(s_snapshot_fallback_count == 1U);
    assert(host_lv_snapshot_capture_count() == 2U);
    assert(host_lv_generic_animation_start_count() == 0U);
    assert(host_lv_generic_animation_count() == 0U);
    assert(host_lv_object_child_count(target) == 0U);
    assert(lv_screen_active() == target);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(target) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(host_lv_snapshot_live_allocation_count() == 0U);
}

typedef enum snapshot_recovery_failure_case
{
    SNAPSHOT_RECOVERY_SCRATCH_CREATE = 0,
    SNAPSHOT_RECOVERY_SCRATCH_LOAD,
    SNAPSHOT_RECOVERY_TARGET_LOAD,
} snapshot_recovery_failure_case_t;

static void _test_snapshot_recovery_failure_case(
    snapshot_recovery_failure_case_t failure_case)
{
    host_lv_system_object_snapshot_t blocker;

    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);

    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);

    const size_t source_children = host_lv_object_child_count(source);
    const size_t target_children = host_lv_object_child_count(target);
    const size_t live_objects = host_lv_live_object_count();
    const size_t live_screens = host_lv_live_screen_count();
    const esp_err_t expected_result =
        failure_case == SNAPSHOT_RECOVERY_SCRATCH_CREATE ?
        ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;

    host_lv_snapshot_fail_next_animation_starts(1U);
    host_lv_fail_next_screen_animation_starts(1U);
    switch (failure_case)
    {
    case SNAPSHOT_RECOVERY_SCRATCH_CREATE:
        host_lv_fail_next_screen_creations(1U);
        break;
    case SNAPSHOT_RECOVERY_SCRATCH_LOAD:
        host_lv_snapshot_fail_next_screen_loads(1U);
        break;
    case SNAPSHOT_RECOVERY_TARGET_LOAD:
        host_lv_fail_screen_load_after(1U);
        break;
    default:
        assert(false);
        break;
    }

    s_completion_count = 0U;
    _diagnostics_reset();
    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_FADE, 220U,
               _transition_completed, &s_completion_context) ==
           expected_result);

    assert(!app_manager_presentation_is_running());
    assert(lv_screen_active() == source);
    assert(s_completion_count == 0U);
    assert(s_diagnostics_event_count == 0U);
    assert(s_snapshot_prepare_start_count == 1U);
    assert(s_snapshot_prepare_finish_count == 1U);
    assert(s_snapshot_fallback_count == 1U);
    assert(host_lv_snapshot_capture_count() == 2U);
    assert(host_lv_generic_animation_start_count() == 0U);
    assert(host_lv_generic_animation_count() == 0U);
    assert(host_lv_object_child_count(source) == source_children);
    assert(host_lv_object_child_count(target) == target_children);
    assert(host_lv_live_object_count() == live_objects);
    assert(host_lv_live_screen_count() == live_screens);
    assert(host_lv_system_object_snapshot(0U, &blocker));
    assert(!blocker.visible);

    _diagnostics_reset();
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_PUSH_LEFT, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(app_manager_presentation_is_running());
    _complete_normally(target);
    assert(!app_manager_presentation_is_running());
    assert(lv_screen_active() == target);
    assert(s_completion_count == 1U);
    assert(host_lv_live_object_count() == live_objects);
    assert(host_lv_live_screen_count() == live_screens);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(target) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(host_lv_system_object_count() == 0U);
    assert(host_lv_snapshot_live_allocation_count() == 0U);
    assert(host_lv_snapshot_live_allocation_bytes() == 0U);
    assert(host_lv_live_object_count() == 0U);
    assert(host_lv_live_screen_count() == 0U);
}

static void _test_snapshot_recovery_failures(void)
{
    for (int failure_case = SNAPSHOT_RECOVERY_SCRATCH_CREATE;
            failure_case <= SNAPSHOT_RECOVERY_TARGET_LOAD; ++failure_case)
    {
        _test_snapshot_recovery_failure_case(
            (snapshot_recovery_failure_case_t)failure_case);
    }
}

static void _test_snapshot_deinit_while_running(void)
{
    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);

    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);

    s_completion_count = 0U;
    _diagnostics_reset();
    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_REVEAL_RIGHT, 220U,
               _transition_completed, &s_completion_context) == ESP_OK);
    assert(host_lv_generic_animation_count() == 1U);
    assert(host_lv_object_child_count(target) == 1U);

    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(s_completion_count == 1U);
    assert(s_diagnostics_event_count == 2U);
    assert(s_diagnostics_events[0] == PRESENTATION_DIAGNOSTICS_STARTED);
    assert(s_diagnostics_events[1] == PRESENTATION_DIAGNOSTICS_COMPLETED);
    assert(host_lv_generic_animation_count() == 0U);
    assert(host_lv_object_child_count(target) == 0U);
    assert(host_lv_system_object_count() == 0U);
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    assert(host_lv_snapshot_live_allocation_count() == 0U);
    assert(host_lv_snapshot_live_allocation_bytes() == 0U);
#endif
}

static void _test_snapshot_target_deleted_during_load(void)
{
    host_lv_system_object_snapshot_t blocker;

    host_lv_reset();
    assert(app_manager_presentation_init() == ESP_OK);

    lv_obj_t *source = app_manager_presentation_create_page_screen();
    lv_obj_t *target = app_manager_presentation_create_page_screen();
    assert(source != NULL && target != NULL);
    assert(app_manager_presentation_load_immediate(source) == ESP_OK);
    assert(lv_obj_add_event_cb(target, _delete_screen_on_load,
                               LV_EVENT_SCREEN_LOAD_START, NULL) != NULL);

    s_completion_count = 0U;
    _diagnostics_reset();
    _prepare_completion(target);
    assert(app_manager_presentation_start(
               source, target, APP_MANAGER_TRANSITION_PUSH_LEFT, 220U,
               _transition_completed, &s_completion_context) ==
           ESP_ERR_INVALID_STATE);
    assert(!app_manager_presentation_is_running());
    assert(s_completion_count == 0U);
    assert(!lv_obj_is_valid(target));
    assert(host_lv_object_child_count(target) == 0U);
    assert(host_lv_generic_animation_count() == 0U);
    assert(host_lv_system_object_snapshot(0U, &blocker));
    assert(!blocker.visible);
    assert(s_diagnostics_event_count == 0U);
    assert(s_snapshot_prepare_start_count == 1U);
    assert(s_snapshot_prepare_finish_count == 1U);
    assert(s_snapshot_fallback_count == 1U);

    assert(app_manager_presentation_load_immediate(
               app_manager_presentation_neutral_screen()) == ESP_OK);
    assert(app_manager_presentation_delete_page_screen(source) == ESP_OK);
    assert(app_manager_presentation_deinit() == ESP_OK);
    assert(host_lv_system_object_count() == 0U);
}
#endif

int main(void)
{
    _test_effect_matrix();
    _test_no_animation_does_not_report_diagnostics();
    _test_barrier_and_fast_forward();
#if TEST_SNAPSHOT_ANIMATION_ENABLED
    _test_snapshot_fallbacks();
    _test_snapshot_and_native_animation_start_failure();
    _test_snapshot_recovery_failures();
    _test_snapshot_deinit_while_running();
    _test_snapshot_target_deleted_during_load();
#endif
    return 0;
}

# Typed-Ops Page Skeletons

These are ownership sketches, not shipped pages. Replace `<app>`, `<page>` and
`<PAGE_ID>`; omit stages a page does not need. Resolve includes, types, failure
codes and layout helpers against the owning headers and the nearest production
page. Layout, text wrapping and touch behavior belong to `lvgl-ui-layout`.

## Static Page

A static page owns UI only. The state is still arena-backed and survives until a
successful STOP, so MOUNT deliberately resets it before rebuilding.

```c
typedef struct
{
    app_ui_page_t page;
    lv_obj_t *value_label;
} <app>_page_state_t;

_Static_assert(sizeof(<app>_page_state_t) <= APP_MANAGER_PAGE_STATE_BYTES,
               "<app> page state exceeds lifecycle arena slot");

static void _<app>_mount(const app_manager_page_context_t *context)
{
    <app>_page_state_t *state = context->state;
    memset(state, 0, sizeof(*state));
    app_ui_page_create(&state->page, "<title>", true);
    /* Build every object below state->page.content. */
}

static void _<app>_unmount(const app_manager_page_context_t *context)
{
    <app>_page_state_t *state = context->state;
    app_ui_page_destroy(&state->page);
    state->value_label = NULL;
}

static const app_manager_page_ops_t s_<app>_ops =
{
    .mount = _<app>_mount,
    .unmount = _<app>_unmount,
};

static const app_manager_page_definition_t s_<app>_definition =
{
    .ops = &s_<app>_ops,
    .memory_size = sizeof(<app>_page_state_t),
};

static const app_manager_page_route_t s_<app>_routes[] =
{
    {
        .page_id = "<PAGE_ID>",
        .definition = &s_<app>_definition,
        .user_data = NULL,
    },
};
```

## Foreground Refresh Page

The timer and subscription are foreground resources. RESUME saves each returned
handle or ownership flag. PAUSE attempts every independent release, preserves
the first error, and retains only resources that were not released successfully.

```c
typedef struct
{
    app_ui_page_t page;
    lv_obj_t *status_label;
    lv_timer_t *refresh_timer;
    event_bus_sub_handle_t subscription;
} <app>_refresh_state_t;

static void _<app>_subscribe(<app>_refresh_state_t *state)
{
    if (state->subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        return;
    }
    const esp_err_t result = event_bus_subscribe(
                                 <SERVICE_MSG>,
                                 <SERVICE_MSG_SUB_TYPE>,
                                 _<app>_service_event, state,
                                 EVENT_BUS_DISPATCH_UI,
                                 &state->subscription);
    if (result != ESP_OK)
    {
        state->subscription = EVENT_BUS_SUB_HANDLE_INVALID;
        LOG_W("<app> subscription failed: %s", esp_err_to_name(result));
    }
}

static void _<app>_resume(const app_manager_page_context_t *context)
{
    <app>_refresh_state_t *state = context->state;
    if (state->refresh_timer == NULL)
    {
        state->refresh_timer = lv_timer_create(_<app>_timer, 1000U, state);
    }
    _<app>_subscribe(state);
}

static esp_err_t _<app>_pause(const app_manager_page_context_t *context)
{
    <app>_refresh_state_t *state = context->state;
    esp_err_t first = ESP_OK;
    esp_err_t result;

    if (state->subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        result = event_bus_unsubscribe(state->subscription);
        if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
        {
            state->subscription = EVENT_BUS_SUB_HANDLE_INVALID;
        }
        else
        {
            first = result;
        }
    }
    if (state->refresh_timer != NULL)
    {
        lv_timer_delete(state->refresh_timer);
        state->refresh_timer = NULL;
    }
    return first;
}
```

The timer is independent of the subscription, so a failed unsubscribe does not
prevent timer cleanup. A later PAUSE receives the retained subscription handle
and retries only that release. If a second resource's safe release depends on
the first one, retain both and skip only that dependent release.

`RESUME` has no error channel, while PAUSE returns only the first retryable
release error. If a foreground acquisition fails, retain an explicit
`subscription_owned` or `timer_owned` flag so the next RESUME can retry it; clear
each flag only after its release succeeds. Initialize timer acquisition before the
subscription when subscribing triggers a callback that depends on the timer.
Validate every timer or subscription creation result rather than assuming it
succeeded.

An `EVENT_BUS_DISPATCH_PUBLISHER` callback can outlive
`event_bus_unsubscribe()`. A Page-owned publisher subscription whose callback
reads `state` must drain with `event_bus_unsubscribe_sync()`; retain its handle on
timeout and clear it only after the drain succeeds. Alternatively, the callback
must not touch Page-owned storage at all. For both dispatch contexts, the event
payload is valid only during the callback; copy any bytes the Page needs later.
The `EVENT_BUS_DISPATCH_UI` example above must unsubscribe on the UI dispatcher
task, where teardown is serialized with queued callbacks.

## Worker Client Page

An App-private worker or session must stop when no Page of that App is foreground.
Work that must survive App inactivity belongs to an independent service/system
owner with its own explicit stop policy. This skeleton is a Page-side client of
such an owner: it requests work in RESUME, consumes copied results, and never
stores a worker handle or passes a Page arena pointer to the worker.

```c
typedef struct
{
    app_ui_page_t page;
    lv_obj_t *status_label;
    lv_timer_t *poll_timer;
    uint32_t request_generation;
    bool request_pending;
} <app>_worker_client_state_t;

static esp_err_t _<app>_submit_worker_request(
    <app>_worker_client_state_t *state)
{
    uint32_t generation = 0U;
    const esp_err_t result = <app>_worker_submit(..., &generation);
    if (result == ESP_OK)
    {
        state->request_generation = generation;
        state->request_pending = true;
    }
    return result;
}

static void _<app>_worker_resume(const app_manager_page_context_t *context)
{
    <app>_worker_client_state_t *state = context->state;
    if (state->poll_timer == NULL)
    {
        state->poll_timer = lv_timer_create(_<app>_poll_worker, 100U, state);
    }
    if (!state->request_pending)
    {
        const esp_err_t result = _<app>_submit_worker_request(state);
        if (result != ESP_OK)
        {
            _<app>_set_worker_status(state, result);
        }
    }
}

static esp_err_t _<app>_worker_pause(
    const app_manager_page_context_t *context)
{
    <app>_worker_client_state_t *state = context->state;
    if (state->poll_timer != NULL)
    {
        lv_timer_delete(state->poll_timer);
        state->poll_timer = NULL;
    }
    return ESP_OK;
}
```

The independent service owns the worker handle and result storage, and its own
stop policy determines whether the request continues while the App is inactive.
The service records the generation returned by submission and retains one copied
result until the Page consumes it. RESUME reuses the saved generation and pending
flag; it does not query and adopt whatever generation the owner happens to hold
now. The poll callback accepts only a result whose generation matches
`state->request_generation`, then clears `request_pending`. A result for any
other generation is stale and cannot write through Page memory or change a newer
request. PAUSE removes only the foreground poll timer and leaves the passive
pending identity intact.

## Typed Argument Page

A new Page instance receives its first argument through START. NEWINTENT updates
an existing instance. Both paths copy the payload into Page state; MOUNT renders
the saved copy and never retains `context->arguments`.

```c
typedef struct
{
    uint64_t alert_key;
} <app>_arguments_t;

_Static_assert(sizeof(<app>_arguments_t) <=
               APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES,
               "<app> arguments exceed the Typed Blob payload");

typedef struct
{
    app_ui_page_t page;
    <app>_arguments_t arguments;
    bool has_arguments;
} <app>_page_state_t;

_Static_assert(sizeof(<app>_page_state_t) <= APP_MANAGER_PAGE_STATE_BYTES,
               "<app> page state exceeds lifecycle arena slot");

static bool _<app>_copy_arguments(const app_manager_typed_blob_t *blob,
                                  <app>_arguments_t *target)
{
    if (target == NULL ||
            blob == NULL ||
            blob->version != APP_MANAGER_TYPED_BLOB_VERSION ||
            blob->type != <APP>_ARGUMENT_ALERT_KEY ||
            blob->size != sizeof(*target))
    {
        return false;
    }
    memcpy(target, blob->payload, sizeof(*target));
    return true;
}

static void _<app>_start(const app_manager_page_context_t *context)
{
    <app>_page_state_t *state = context->state;
    state->has_arguments =
        _<app>_copy_arguments(context->arguments, &state->arguments);
}

static void _<app>_mount(const app_manager_page_context_t *context)
{
    <app>_page_state_t *state = context->state;
    app_ui_page_create(&state->page, "<title>", true);
    /* Build UI under state->page.content, then render state->arguments when
     * state->has_arguments is true. */
}

static void _<app>_unmount(const app_manager_page_context_t *context)
{
    <app>_page_state_t *state = context->state;
    app_ui_page_destroy(&state->page);
    /* Clear every separately stored LVGL pointer here. */
}

static void _<app>_new_intent(const app_manager_page_context_t *context)
{
    <app>_page_state_t *state = context->state;
    <app>_arguments_t next;
    const bool has_arguments =
        _<app>_copy_arguments(context->arguments, &next);
    if (has_arguments)
    {
        state->arguments = next;
    }
    else
    {
        memset(&state->arguments, 0, sizeof(state->arguments));
    }
    state->has_arguments = has_arguments;
    if (context->screen != NULL)
    {
        _<app>_render_arguments(state);
    }
}

static const app_manager_page_ops_t s_<app>_ops =
{
    .start = _<app>_start,
    .mount = _<app>_mount,
    .new_intent = _<app>_new_intent,
    .unmount = _<app>_unmount,
};
```

An invalid or absent initial argument leaves `has_arguments` false; MOUNT must
render a defined empty or unavailable state instead of dereferencing stale
payload data. NEWINTENT replaces the complete prior intent even when the new
argument is absent or invalid, so always clear the copied value and update
`has_arguments` before rendering. The target Page must be in the App's static
route table. Use `app_ui_request_*` or `app_manager_navigate_async()` from LVGL
callbacks; never call the synchronous `app_manager_navigate()` on the UI worker.
A custom completion's context is borrowed, not copied, and can outlive this Page:
pass `NULL` or explicitly owned storage, never `state` or a stack object. The
callback runs only after successful admission, so release a newly allocated
context yourself when `app_manager_navigate_async()` returns an error.

## App Registration

Existing Apps declare their catalog entry in `layers/apps/app_catalog.cmake` and
their sources, dependencies, resources and capabilities in the owning
`app.cmake`. A new App also needs a stable `APP_MANAGER_ID_<APP>` in
`app_manager_ids.h`, a matching `app_id` to `<app>_app` mapping in
`mt_app_dir_name()`, and a product-manifest selection. Define the complete
`app.cmake` field set and write `""` for intentional empties. `mt_app_read()`
requires nonempty `MT_APP_ID`, `MT_APP_SOURCES`, and `MT_APP_REQUIRED`; explicit
empty values keep the remaining fields unambiguous:

```cmake
set(MT_APP_ID <app>)
set(MT_APP_SOURCES src/<app>_app.c)
set(MT_APP_INCLUDE_DIRS src)
set(MT_APP_PRIVATE_REQUIRES app_core app_theme freertos lvgl__lvgl mt_log)
set(MT_APP_CAPABILITIES "")
set(MT_APP_RESOURCE_MANIFEST resource_manifest.cmake)
set(MT_APP_REQUIRED FALSE)
set(MT_APP_HIDDEN FALSE)
set(MT_APP_RESET_HOOK "")
```

`MT_APP_HIDDEN` must match the descriptor flag. The descriptor is exported once
from the App entry file:

```c
APP_MANAGER_APP_EXPORT_META(<app>, APP_IMAGE_<APP>_ICON, "<name>",
                            APP_MANAGER_ID_<APP>, "<root-page-id>",
                            <app-flags>, s_<app>_routes,
                            <launcher-order>, "<launcher-subtitle>");
```

Use `APP_MANAGER_APP_FLAG_NONE` for a normal App or
`APP_MANAGER_APP_FLAG_HIDDEN` for a hidden one, matching `MT_APP_HIDDEN` in
`app.cmake`. PINNED is a separate policy that should be reserved for the product
that deliberately requires residency. Do not add a hand-written descriptor, a
runtime Page, or a second source list. For an optional App, keep
`MT_APP_REQUIRED FALSE`, declare its capabilities, and guard every launcher or
settings entry with `app_manager_builtin_has_app()`.

---
name: app-development
description: MicroTech built-in application development workflow for layers/apps, including App Manager Page ops and raw handlers, lifecycle ownership, static routes, app_ui composition, navigation, Typed Blob arguments, subscriptions, workers, private state, and host coverage. Use when creating, modifying, or reviewing an App or Page in layers/apps, or when working with APP_MANAGER_APP_EXPORT, Page lifecycle callbacks, route ownership, or application resource cleanup; use esp-idf for CMake and configuration, esp32 for board and peripheral facts, lvgl-integration for display and touch pipeline facts, debug-esp32s3 for live-device diagnosis, and validate-firmware for validation scope.
---

# MicroTech App Development

Apply this workflow to built-in applications under `layers/apps`. Follow
`AGENTS.md` and `doc/code-style.md`; verify current types and service contracts
instead of copying APIs or capacities from this skill.

## Sources of Truth

- Read section 12, "应用开发契约", in `layers/app_manager/README.md`
  completely for Page ops, context validity, lifecycle ownership, navigation,
  arguments, and evidence.
- Read `layers/apps/README.md`, the target App route table, and the nearest
  production Page. Prefer Weather as the typed-ops and Typed Blob reference.
- Read `app_manager_types.h`, `app_manager.h`, `app_ui.h`, and the target
  service's public header before changing interfaces or ownership.
- Read the owning host CMake/README plus App Manager and cross-layer tests when
  behavior spans lifecycle, navigation, callbacks, or resources.

## Workflow

1. Classify the change as a new App, new Page, or existing Page update.
2. Classify the Page as static, refresh, session, worker, or argument driven,
   then select only the lifecycle stages required by its owned resources.
3. Choose typed Page ops for ordinary lifecycle stages. Keep a raw handler only
   when direct message dispatch materially simplifies a complex Page.
4. Inventory every UI object, timer, subscription, worker, service session,
   snapshot, argument, and retained handle by acquisition and release stage.
5. Define only instance state in Page memory and check it against
   `APP_MANAGER_PAGE_STATE_BYTES`; allocate large payloads separately.
6. Implement MOUNT/UNMOUNT first, then required RESUME/PAUSE, START/STOP, and
   NEWINTENT behavior. Preserve the first cleanup error and failed handle.
7. Bind the immutable definition through an App-owned static route. Export an
   App descriptor only for a new App and add new sources explicitly to
   `APP_SRCS` without recursive globbing.
8. Use `app_ui_request_*` for ordinary navigation and
   `app_manager_navigate_async()` for arguments, custom transitions, or
   completion. Validate application-specific Typed Blob type and exact size.
9. Add focused host coverage and use `validate-firmware` to select sanitizer,
   build, size, and hardware evidence.

## Ownership Rules

- Initialize only nonzero sentinels in START; Page state is already zeroed.
- Create UI only below the Page Screen in MOUNT. Destroy it and clear every
  LVGL pointer in UNMOUNT.
- Start foreground resources in RESUME and stop them in PAUSE. Support a
  compensating RESUME-to-PAUSE before the target becomes stably visible.
- Return cleanup failures from ops PAUSE/STOP. Raw handlers report them through
  `app_manager_this_page_report_cleanup_error()`; do not mix both paths.
- Do not repeat PAUSE work in STOP. Keep valid handles when release fails so a
  later lifecycle request can retry ownership cleanup.
- Treat ops context and arguments as callback-scoped. Copy any argument payload
  needed after the callback; never retain a `this_page_*` accessor result as a
  general cross-task capability.
- Keep blocking file, audio, parsing, network, and hardware work off the UI
  worker. Update UI only through owned snapshots or the UI dispatcher.

## Prohibited Patterns

- Do not create LVGL objects in START or use `lv_screen_active()` as a Page root.
- Do not create runtime Pages outside the owning App route table.
- Do not clear a subscription, worker, or service handle after failed cleanup.
- Do not add another LVGL task, tick source, Screen manager, or direct hardware
  ownership to an App.
- Do not modify ESP-IDF, `managed_components`, or BSP third-party code to hide
  an application defect.

## Completion Evidence

Report changed Apps, Pages, routes, Page-state sizes, resource ownership,
cleanup retry behavior, and every executed test. State separately whether
sanitizers, ESP-IDF build, size, on-board UI/touch, and long-running resource
checks ran. Host success does not prove hardware display, touch, DMA, radio,
power, or performance behavior.

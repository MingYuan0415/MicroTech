---
name: app-development
description: MicroTech built-in application development workflow for layers/apps, including App Manager Page ops and raw handlers, lifecycle ownership, static routes, navigation, Typed Blob arguments, app_ui composition, subscriptions, workers, and private state. Use when creating, modifying, or reviewing an App or Page in layers/apps; use esp32 for board and peripheral facts and lvgl-integration for display-pipeline ownership.
---

# MicroTech App Development

Apply this workflow to built-in applications under `layers/apps`. Follow
`AGENTS.md` and `doc/code-style.md`; verify current types, contracts, and
capacities in the owning headers instead of copying them from this skill.

## Sources of Truth

- Section 12, "应用开发契约", in `layers/app_manager/README.md` is the
  authoritative contract for Page ops, context validity, lifecycle ownership,
  navigation, and arguments.
- Read `layers/apps/README.md`, the target App route table, and the nearest
  production Page. Prefer Weather as the typed-ops and Typed Blob reference.
- Read `app_manager_types.h`, `app_manager.h`, `app_ui.h`, and the target
  service's public header before changing interfaces or ownership.
- Image assets follow the SVG-first pipeline in
  [references/asset-authoring.md](references/asset-authoring.md); no committed
  PNG sources exist.

## Workflow

1. Classify the change (new App, new Page, existing Page update) and the Page
   model (static, refresh, session, worker, argument driven); implement only
   the lifecycle stages its owned resources require.
2. Choose typed Page ops for ordinary stages; keep a raw handler only when
   direct message dispatch materially simplifies the Page.
3. Inventory every UI object, timer, subscription, worker, snapshot, argument,
   and retained handle by acquisition and release stage.
4. Keep only instance state in Page memory (`APP_MANAGER_PAGE_STATE_BYTES`);
   allocate large payloads separately.
5. Implement MOUNT/UNMOUNT first, then required RESUME/PAUSE, START/STOP, and
   NEWINTENT behavior. Preserve the first cleanup error and failed handle.
6. Bind the immutable definition through an App-owned static route; export an
   App descriptor only for a new App and add sources to `APP_SRCS` explicitly.
   When the App owns icons, author SVG sources, register them in
   `resource_manifest.cmake` plus `app_image_ids.h`, and run
   `idf.py reconfigure` (see references/asset-authoring.md).
7. Navigate through `app_manager_navigate_*` or `app_ui_request_*`; validate
   Typed Blob type and exact size before use.
8. Add focused host coverage for the changed behavior; validation scope
   follows the `AGENTS.md` default workflow.
9. Cover UI behavior with simulator scenarios: reach subpages deterministically
   via `sim.navigate` `page`, assert tree fields through dotted paths
   (`scroll.y`, `flags.scrollable`, numeric `{"min": N, "max": M}`), and use
   `drag` steps to prove the page's scroll policy both directions. `sim.step`
   accepts only multiples of 33 ms and every RPC result must be asserted: a
   rejected call advances no frames and masquerades as an unresponsive UI.
    Derive tap coordinates from the dumped tree geometry, review a screenshot
    for every new page, and remember that pages with looping animations (for
    example `LV_LABEL_LONG_SCROLL_CIRCULAR` overflow) never satisfy `wait_idle`.
    Before declaring a page done, run the `ui-review` gate: a per-page state
    matrix (empty/loading/populated/disabled/error/transient) screenshotted and
    linted by `sim/tools/review_pages.py` (single-line fit, unreachable control,
    washed-out disabled, fallback icon), not one happy-path screenshot.

## Ownership Rules

- Initialize only nonzero sentinels in START; Page state is already zeroed.
- Page state is zeroed only when the instance is allocated; a pop-back remounts
  with the same arena. Re-initialize render caches and defaults at the top of
  MOUNT, and keep values that must survive remounts (a selected duration, a
  session choice) in App-scope statics behind internal accessors.
- NEWINTENT runs on the App worker thread: copy payloads only and never call
  LVGL from it (that deadlocks against the UI thread). Apply visual changes in
  MOUNT/RESUME.
- Structure a multi-page App as one file per page plus an
  `<app>_internal.h` exposing route id macros, extern page definitions, and
  shared helpers; register every source explicitly in `APP_SRCS`.
- Create UI only below the Page Screen in MOUNT; destroy it and clear every
  LVGL pointer in UNMOUNT.
- Start foreground resources in RESUME, stop them in PAUSE; support a
  compensating RESUME-to-PAUSE before the target becomes stably visible.
- Return cleanup failures from ops PAUSE/STOP (raw handlers report through
  `app_manager_this_page_report_cleanup_error()`; do not mix both paths).
- Do not repeat PAUSE work in STOP; keep valid handles when release fails so a
  later lifecycle request can retry.
- Treat ops context and arguments as callback-scoped; copy any payload needed
  after the callback.
- Keep blocking file, audio, parsing, network, and hardware work off the UI
  worker; update UI only through owned snapshots or the UI dispatcher.

## Prohibited Patterns

- Do not create LVGL objects in START or use `lv_screen_active()` as a Page root.
- Do not render copy that announces unimplemented features or absent hardware
  (for example "firmware update: unavailable" or "no magnetometer, no
  compass"). Show implemented behavior and live service state only; a
  disabled control needs no explanatory label.
- Do not call LVGL from NEWINTENT or any other worker-thread callback.
- Do not apply label APIs to non-label objects; a button caption is its child
  label.
- Do not create runtime Pages outside the owning App route table.
- Do not clear a subscription, worker, or service handle after failed cleanup.
- Do not add another LVGL task, tick source, Screen manager, or direct hardware
  ownership to an App.
- Do not modify ESP-IDF, `managed_components`, or BSP third-party code to hide
  an application defect.

---
name: app-development
description: Create, modify, or review MicroTech Apps and Pages in layers/apps, including lifecycle ownership, static routes, typed arguments, subscriptions, and workers. Use for application contracts, not board wiring or display-pipeline internals.
---

# MicroTech App Development

Apply this workflow to built-in applications under `layers/apps`. Follow
`AGENTS.md` and `doc/code-style.md`; verify current types, contracts, and
capacities in the owning headers instead of copying them from this skill.

## Sources of Truth

- Section 12, "Application Development Contract", in
  `layers/app_manager/README.md` is the authoritative contract for Page ops,
  context validity, lifecycle ownership, navigation, and arguments.
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
2. Use typed Page ops for new Pages; raw handlers are the compatibility path
   for existing direct message dispatch.
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
7. Use asynchronous navigation from LVGL callbacks (`app_ui_request_*` or
   `app_manager_navigate_async`); validate Typed Blob version, type, and exact
   size before decoding with `memcpy`.
8. Add focused host coverage for the changed behavior; validation scope
   follows the `AGENTS.md` default workflow.
9. For visible or interaction changes, use `ui-review` at the affected-page
   scope. Simulator commands, scenario assertions, and session operation live
   in `sim/README.md`; layout decisions use `lvgl-ui-layout`. Pure logic changes
   with no UI impact do not automatically require a screenshot matrix.

## Ownership Rules

- Initialize only nonzero sentinels in START; Page state is already zeroed.
- Page state survives UNMOUNT/remount until successful STOP. Reset mount-local
  LVGL pointers and render caches without discarding instance selections or
  copied arguments. Use App-scope state only for genuinely shared data, and
  service state for service-owned sessions; do not promote instance data to
  statics merely to survive remount.
- Lifecycle callbacks, including NEWINTENT, run on the UI worker. NEWINTENT
  validates and copies arguments; when mounted it may update its own UI, and
  when unmounted it retains data for MOUNT/RESUME. It must not block.
- Prefer one file per substantial page with a narrow `<app>_internal.h` for
  routes and shared helpers; do not split a cohesive small page just for shape.
  Register every source explicitly in `app_sources.cmake`.
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
  worker; update UI only through owned snapshots or the UI dispatcher. LVGL
  callbacks run on the PSRAM-stacked `lvgl` worker, so never commit NVS or
  write/erase internal flash there: queue the one-shot storage work on
  `chore_service` or the owning service worker (see the `esp32` skill).

## Prohibited Patterns

- Do not create LVGL objects in START or use `lv_screen_active()` as a Page root.
- Do not present unimplemented behavior as an available feature. Explain a
  disabled control or unavailable service when the reason helps the user act;
  avoid placeholder feature announcements unrelated to the task.
- Do not call LVGL from background workers; use the UI dispatcher or snapshots.
- Do not apply label APIs to non-label objects; a button caption is its child
  label.
- Keep required external identifiers readable at their decision surface;
  text-treatment choices live in `lvgl-ui-layout`.
- Do not create runtime Pages outside the owning App route table.
- Do not clear a subscription, worker, or service handle after failed cleanup.
- Do not add another LVGL task, tick source, Screen manager, or direct hardware
  ownership to an App.
- Do not modify ESP-IDF, `managed_components`, or BSP third-party code to hide
  an application defect.

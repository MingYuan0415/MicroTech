---
name: app-development
description: Create, modify, or review MicroTech Apps and Pages in layers/apps, including lifecycle ownership, static routes, typed arguments, subscriptions, and workers. Use for application contracts, not board wiring or display-pipeline internals.
---

# MicroTech App Development

Apply this workflow to built-in applications under `layers/apps`. Follow
`AGENTS.md`, `doc/verification.md`, and `doc/code-style.md`; verify current
types, contracts, and capacities in the owning headers instead of copying them
from this skill.

## Sources of Truth

- Section 12, "Application Development Contract", in
  `layers/app_manager/README.md` is the authoritative contract for Page ops,
  context validity, lifecycle ownership, navigation, and arguments. Section 1
  of that document and `layers/apps/README.md` additionally require a
  background Page to retain only passive state, not an active timer,
  subscription, input registration, worker, or active service session.
- Read `layers/apps/README.md`, the target App route table, and the nearest
  production Page. Prefer Weather as the typed-ops and Typed Blob reference.
- Read `app_manager_types.h`, `app_manager.h`, `app_ui.h`, and the target
  service's public header before changing interfaces or ownership.
- Image assets follow the SVG-first pipeline in
  [references/asset-authoring.md](references/asset-authoring.md); no committed
  PNG sources exist.
- Resource lifetime, teardown ordering and stale-async-result handling are in
  [references/application-ownership.md](references/application-ownership.md).
- Adaptable typed-ops and worker skeletons are in
  [assets/page-skeletons.md](assets/page-skeletons.md). They show ownership and
  lifecycle shape, not layout or a shipped implementation.

## Framework Facts and Their Consequences

| Framework fact | Application consequence |
| --- | --- |
| Page memory is a fixed arena slot defined by `APP_MANAGER_PAGE_STATE_BYTES`; exactly `memory_size` bytes are zeroed when an instance is created | Keep only instance-private data in it, set `memory_size = sizeof(page_state)`, add a compile-time bound, and allocate large or variable payloads separately. |
| A successful STOP releases the instance and its memory; UNMOUNT only removes the mounted Screen | Decide explicitly whether the next MOUNT continues instance state or reinitializes it. Clear every LVGL pointer in UNMOUNT without assuming the arena itself was cleared. A STOP cleanup failure preserves the instance. Back/exit rollback can MOUNT and RESUME that same instance before retrying STOP, so do not irreversibly clear retained argument, selection, or remount-input state while returning failure. |
| `app_manager_page_context_t` and its `arguments` pointer are callback-scoped | Copy any Typed Blob payload that must outlive NEWINTENT or any other lifecycle callback. Do not retain `context`, `screen` or `arguments` in Page state. |
| `context->screen` exists only while mounted; START and STOP receive `NULL` | Create UI only in MOUNT under that Screen, and dereference LVGL pointers only while mounted. |
| NEWINTENT is a side channel on an existing instance and fully replaces its intent | Validate and copy new arguments in both mounted and unmounted cases. An absent or invalid replacement clears the retained argument state; refresh immediately only when the Screen exists, otherwise apply the replacement in MOUNT or RESUME. |
| Typed ops PAUSE/STOP return `esp_err_t`; raw handlers report through `app_manager_this_page_report_cleanup_error()` | Use one cleanup channel per definition. Clear a handle only after its release succeeds so the lifecycle can retry. |
| The route table is immutable and every Page instance is created through an App-owned route | Keep definitions and routes `const`; use file-local `static const` unless another translation unit must reference the definition. Do not create runtime Pages, and keep Page IDs unique within the App. |
| `app_manager_navigate_async()` copies its request but borrows the completion `context`; the completion runs only after successful admission | Do not pass Page arena, callback-stack, or other Page-lifetime storage. Prefer `NULL` or explicitly owned storage that outlives completion; if admission fails, no callback runs, so release a newly allocated context on that error path. |

The complete contract remains Section 12 of `layers/app_manager/README.md`; the
public types in `app_manager_types.h` override this summary.

## Workflow

1. Classify the change (new App, new Page, existing Page update) and the Page
   model (static, refresh, session, worker, argument driven); implement only
   the lifecycle stages its owned resources require.
2. Use typed Page ops for new Pages; raw handlers are the compatibility path
   for existing direct message dispatch.
3. Inventory every UI object, timer, subscription, worker, snapshot, argument,
   and retained handle by acquisition and release stage.
4. Keep only instance state in Page memory, set `memory_size` to that state's
   exact size, keep it within `APP_MANAGER_PAGE_STATE_BYTES`, and allocate large
   payloads separately.
5. Implement MOUNT/UNMOUNT first, then required RESUME/PAUSE, START/STOP, and
   NEWINTENT behavior. Preserve the first cleanup error and failed handle.
6. Bind the immutable definition through an App-owned static route; export an
   App descriptor only for a new App. Add its stable ID to
   `app_manager_ids.h` and its ID to `MT_APP_CATALOG_IDS`; use the standard
   `<app_id>_app` directory, or update `mt_app_dir_name()` for an exception.
   Define all required `app.cmake` fields: `MT_APP_ID`, `MT_APP_SOURCES`,
   `MT_APP_INCLUDE_DIRS`, `MT_APP_PRIVATE_REQUIRES`, `MT_APP_CAPABILITIES`,
   `MT_APP_RESOURCE_MANIFEST`, `MT_APP_REQUIRED`, `MT_APP_HIDDEN`, and
   `MT_APP_RESET_HOOK`, using `""` where a field is intentionally empty. Select
   it in the intended product manifest; a catalog entry alone does not compile
   it. Keep `MT_APP_HIDDEN` synchronized with the descriptor's
   `APP_MANAGER_APP_FLAG_HIDDEN`.
   When the App owns icons, author SVG sources, register them in
   `resource_manifest.cmake` plus `app_image_ids.h`, and run
   `idf.py reconfigure` (see references/asset-authoring.md).
7. Use asynchronous navigation from LVGL callbacks (`app_ui_request_*` or
   `app_manager_navigate_async`); validate Typed Blob version, type, and exact
   size before decoding with `memcpy`. A custom completion can run after Page
   teardown, so never pass Page arena or a callback stack pointer as its context.
8. Add focused host coverage for the changed behavior; validation scope
   follows the `doc/verification.md` workflow.
9. For visible or interaction changes, use `ui-review` at the affected-page
   scope. Simulator commands, scenario assertions, and session operation live
   in `sim/README.md`; layout decisions use `lvgl-ui-layout`. Pure logic changes
   with no UI impact do not automatically require a screenshot matrix.

## Ownership Rules

- START receives freshly zeroed storage for exactly `memory_size` bytes. Set
  every nonzero sentinel and initialize every START-acquired resource there.
- Page state survives UNMOUNT/remount until successful STOP. A Page with only
  MOUNT state may reset the whole state in MOUNT; a Page with START state must
  preserve its declared retained fields and reset or rebuild only mount-local
  fields. Never rely on arena contents that were not deliberately retained.
- Lifecycle callbacks, including NEWINTENT, run on the UI worker. NEWINTENT
  validates the replacement intent, copies valid data, and clears any invalid or
  absent copy instead of retaining the previous argument. It may update its own
  mounted UI and must not block.
- Prefer one file per substantial page with a narrow `<app>_internal.h` for
  routes and shared helpers; do not split a cohesive small page just for shape.
  Register every source explicitly in the App's `app.cmake`.
- Create UI only below the Page Screen in MOUNT; destroy it and clear every
  LVGL pointer in UNMOUNT.
- Start foreground resources in RESUME, stop them in PAUSE; support a
  compensating RESUME-to-PAUSE before the target becomes stably visible.
- Return cleanup failures from ops PAUSE/STOP (raw handlers report through
  `app_manager_this_page_report_cleanup_error()`; do not mix both paths).
- STOP releases retained START state and does not repeat resources whose
  PAUSE path already released them. Keep valid handles when either release
  stage fails so a later lifecycle request can retry. If STOP returns an error,
  preserve the retained argument, selection, and rebuild inputs that a rollback
  MOUNT/RESUME may need; only final successful STOP may invalidate them.
- Treat ops context and arguments as callback-scoped; copy any payload needed
  after the callback.
- Keep blocking file, audio, parsing, network, and hardware work off the UI
  worker; update UI only through owned snapshots or the UI dispatcher. LVGL
  callbacks run on the PSRAM-stacked `lvgl` worker, so never commit NVS or
  write/erase internal flash there: queue the one-shot storage work on
  `chore_service` or the owning service worker (see the `esp32` skill).

### State and Resource Placement

| Data or resource | Owner | Required lifetime | Release or invalidation point |
| --- | --- | --- | --- |
| LVGL objects, animation targets and Screen-local render cache | Page | MOUNT to UNMOUNT | Delete children through the Page root, then clear every stored LVGL pointer in UNMOUNT. |
| Instance selection, copied arguments, filter/calculation state intended across remount | Page arena | START to successful STOP | Reset only by an explicit policy; never free the arena block. |
| Timer, UI subscription, Page worker and active service session | Page | RESUME to PAUSE | Stop in reverse acquisition order. Attempt independent releases, preserve the first error, and retain only failed resources or resources whose safe release depends on them. |
| Work that must continue while the Page is backgrounded or its App is inactive | Independent service/system owner | According to that service's own lifecycle | Keep only passive copied state or an invalid/idle handle in the Page arena. An App-private worker or session must stop in PAUSE and must not survive the App becoming backgrounded. |
| Large decoded payload, network/file buffer, page worker stack | Page or owning service, allocated outside the arena | Shortest interval that needs it | Keep pointer and size together; release in the stage that acquired it. Never put the allocation in the arena. |
| Data shared by several Pages of one App | App-level owner | App lifetime | Use a deliberate App/service API rather than promoting one Page's instance state to a file-static. |
| Asynchronous completion from a worker or service | Requesting owner plus an identity/generation check | Until completion is consumed or proven stale | Publish into the owner's mailbox; never write through a paused/unmounted Page pointer. |
| Read-only tables and immutable descriptors | Firmware | Whole firmware lifetime | Keep in Flash/`.rodata`; do not copy into Page state merely for convenience. |

A Page may own a foreground subscription only from RESUME to PAUSE. Only an
`EVENT_BUS_DISPATCH_UI` handler may assume the Page Screen and visible widgets
exist; an `EVENT_BUS_DISPATCH_PUBLISHER` handler runs in the publishing task and
must not touch LVGL. When work must continue after PAUSE, complete it
synchronously first or move it to an independent service/system owner whose own
lifecycle handles App inactivity. An App-private worker or session is not such an
owner: it must stop when its last Page pauses. A Page may retain only passive
copied state or an inactive prediction from an independent owner. START/STOP may
allocate and release passive Page state, but it must not preserve an active
timer, subscription, input registration, worker, or service session across a
successful PAUSE. Never use a subscription to keep paused Page LVGL pointers
alive.

### Worker and Session Teardown

For an App-owned worker, session or IPC endpoint, use
[references/application-ownership.md](references/application-ownership.md) and
verify all of these:

1. Every partial-initialization path unwinds only resources that were actually
   initialized; a failed instance is never published as usable. On teardown,
   continue independent releases after an error and preserve the first error.
2. Teardown closes ingress before stopping producers, drains or releases queued
   payloads, waits for callbacks or workers to acknowledge exit, and only then
   frees their storage. Still attempt independent cleanup after one release
   fails; retain only the failed resource and any resource that cannot be safely
   released without it. Return the first cleanup error. Do not synchronously
   join a task from its own callback.
3. Allocations, I/O and waits happen outside scheduler locks. Validate the
   instance identity again after reacquiring a lock.
4. Late completions carry an instance/generation identity and are discarded or
   delivered as a copied result without dereferencing Page memory.
5. A release failure leaves the handle and ownership evidence intact for a
   later PAUSE/STOP retry. Do not claim that grouping fields into one struct,
   deleting a static reservation or freeing an idle singleton proves peak-memory
   recovery; state the measured time interval and assumptions.

## Optional Apps and Entry Closure

Apps are not all compiled into every product. A product manifest
(`products/<product>/product.cmake`) selects a subset of `app_catalog.cmake`;
required Apps are Home, Menu and Settings. When adding or reviewing an optional
App:

- Declare `MT_APP_REQUIRED FALSE` in its `app.cmake` and list every required
  board capability in `MT_APP_CAPABILITIES`; a product that lacks the
  capability must not select the App.
- Check every entry that navigates to the App with
  `app_manager_builtin_has_app()` before creating a clickable control. Never
  leave a Home card, Settings row or storage shortcut that navigates to an App
  absent from the build.
- If the App owns persisted state, add its reset hook to `app.cmake` and make
  its persistence header visible to `apps_persistence.c`; the shared
  factory-reset path invokes the selected hooks generated from the product.
- A hidden App (`APP_MANAGER_APP_FLAG_HIDDEN`) can still be optional; the flag
  only removes it from the launcher and recent-task surfaces, not from the
  build. Keep `MT_APP_HIDDEN` in `app.cmake` synchronized with the descriptor
  flag; repository configuration tests enforce that agreement.
- Cover both the selected and unselected composition in host tests. A minimal
  product fixture (`products/minimal`) builds only the shared `common` layer plus
  Home, Menu and Settings; it must not link any other App.

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
- Do not keep a callback-scoped `context`, Screen, or Typed Blob pointer beyond
  its documented lifetime. Route `user_data` may be retained only when its
  immutable storage covers the firmware lifetime and its ownership is explicit.
- Do not make a generic "app-static" copy of Page data without naming the owner,
  the release point and the stale-result policy.
- Do not treat `volatile`, a cleared pointer or an unverified cleanup
  acknowledgement as a worker-lifetime barrier.
- Do not modify ESP-IDF, `managed_components`, or BSP third-party code to hide
  an application defect.

## App-Focused Verification

Use the tiers in `doc/verification.md` for commands and scope. Before handing off, record
which of these App-local checks were completed and which remain unverified:

- App IDs, catalog membership, product selection, and manifest hidden state are
  wired; every route, Page ID and `root_page_id` resolves to an immutable
  definition; optional-App entries are guarded by
  `app_manager_builtin_has_app()`.
- Definitions and routes are file-local `static const` unless referenced across
  translation units, in which case the definition is exported `const` and
  declared `extern`.
- `memory_size == sizeof(page_state) <= APP_MANAGER_PAGE_STATE_BYTES`, and any
  separately allocated payload has one acquisition path, a size bound and a
  release path.
- MOUNT creates UI only under `context->screen`; UNMOUNT clears every stored
  LVGL pointer and does not clear instance data that must survive remount.
- Each timer, subscription, worker and session has a symmetric release path;
  cleanup attempts independent releases, returns the first cleanup error and
  retains only failed resources or resources whose safe release depends on them.
  A Page-owned `EVENT_BUS_DISPATCH_PUBLISHER` callback either cannot access Page
  storage or is drained synchronously before teardown.
- A background Page and background App retain no active timer, subscription,
  input registration, worker, or service session. Work that survives App
  inactivity belongs to an independent service/system owner.
- NEWINTENT validates version, type and exact payload size, copies a valid
  replacement, clears invalid or absent intent, and behaves correctly both while
  mounted and while unmounted.
- Background completions carry generation/identity checks; stale results cannot
  dereference Page memory or update a different instance. Async navigation
  completion never receives Page arena or callback-stack storage as `context`.
- Optional-App composition was checked with the relevant product selection, and
  a hidden App was not mistaken for an unbuilt App.
- Visible changes followed `ui-review` and `lvgl-ui-layout`; pure logic changes
  used the affected host suite without adding a screenshot matrix.

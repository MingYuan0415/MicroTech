# Application-Owned Runtime and Teardown

This reference covers App/service workers, Page foreground resources, service
sessions, IPC endpoints and asynchronous completions. Page registration,
arguments and lifecycle ordering remain authoritative in Section 12 of
`layers/app_manager/README.md`; its Section 1 invariant forbids a background
Page from retaining an active timer, subscription, input registration, worker,
or service session.

## Ownership Record

Before changing a runtime, write down these facts in the change or test plan:

| Question | Answer required |
| --- | --- |
| Who creates the object? | A Page, an App-level owner, a service, or the framework |
| Who may access it? | UI worker, service worker, ISR, or a named set of tasks |
| What is its final possible access? | Callback return, queue drain, acknowledgement or generation match |
| What is its allocation region? | Page arena, internal heap, PSRAM, task-owned stack or static storage |
| What releases it? | The exact lifecycle stage or service operation |
| What happens after partial failure? | Which handles stay valid and which operation can retry |

Keep independent lifetimes separate. A static struct still reserves memory, and
placing flags, buffers and handles in one struct does not by itself make their
lifetimes equal.

## Initialization and Rollback

1. Allocate the context and initialize each field to a known sentinel.
2. Create IPC and worker resources in dependency order. Do not publish the
   context or register callbacks until the instance is usable.
3. On any failure, unwind only resources whose creation succeeded, in reverse
   order. Preserve the first error and continue releasing independent resources.
4. Keep a rejected or partially initialized instance invisible to other tasks;
   no external side effect may survive a failed transaction.
5. If the framework or service owns a stack or handle, follow that owner's
   destroy/detach contract rather than freeing the storage directly.

## Teardown Barrier

The order below is a checklist, not a reason to add unnecessary locks:

1. Mark the instance as closing so new ingress is rejected.
2. Stop or unsubscribe producers and cancel or invalidate pending requests.
3. Drain or release queued payloads according to their owner.
4. Wait for callbacks and dependent workers to acknowledge exit.
5. Free worker-owned storage, IPC and the context last.
6. Record whether each resource was released. Clear or retain its stored handle
   according to that API's retry and idempotency rules; never discard ownership
   evidence for a failed release.

A failure does not justify skipping independent releases. Preserve the first
error, retain the failed handle and anything that cannot be safely released
without it, and let a later PAUSE/STOP retry those resources. Do not clear a
failed handle merely because another resource's release failed.

A STOP failure can roll back to the same Page instance: the framework may MOUNT
and RESUME it before retrying STOP. Until STOP can succeed, retain every
argument, selection, and rebuild input needed by that return path. Irreversible
invalidation is valid only once no callback will use the instance again.

A cleared pointer is not a barrier. A queue acknowledgement proves only that the
message was consumed, not that a task has exited. Do not synchronously close or
join from a callback that the close operation is waiting on. Scheduler locks do
not exclude interrupts or other cores, and `volatile` is not lifetime
synchronization.

## Asynchronous Results

Use an instance or generation identity whenever a completion can outlive the
request that produced it:

- Capture the identity when submitting the request.
- On completion, reacquire the owner and compare the identity before publishing.
- Publish only a copied value or a thread-safe snapshot, not a pointer into Page
  arena memory.
- Treat an older generation, a released instance and a duplicate completion as
  stale, without touching Page state.
- Keep the result record for the query lifetime promised by the API; garbage
  collection is not an implicit part of the contract.

`level_app.c` is the production reference for a generation mailbox: a bounded
chore writes results outside the Page arena, and the UI consumes only the
matching generation after remount. `weather_ui` and the music pages show the
snapshot/subscription side of the same rule. A completion that outlives the Page
request must target an App/service-owned mailbox or copied result. Do not point
an asynchronous callback at a Page arena address merely because the memory is not
freed until STOP.

`app_manager_navigate_async()` copies the request but borrows its completion
`context`; it runs later on the UI worker and is not copied. BACK or EXIT cleanup
can UNMOUNT, STOP, and free the source Page before completion runs. Do not pass a
Page arena address or a callback stack object as that context. Immutable lifetime
storage and App-owned state can be used only when that owner remains valid; prefer
`NULL` for status-only completion and state who invalidates any non-NULL context.
The completion runs only after successful admission. If submission fails, no
callback will release a dynamically allocated context; release it on that error
path, or keep the context pre-owned and stable until the admitted completion runs.

## Subscription Placement

Use RESUME/PAUSE for a foreground subscription. Release independent
subscriptions in reverse acquisition order, retain only handles whose release
failed, and preserve the first error.

An `EVENT_BUS_DISPATCH_UI` callback runs on the UI worker, so while the Page is
foreground it may assume the current Page Screen and visible widgets exist. An
`EVENT_BUS_DISPATCH_PUBLISHER` callback runs in the publishing task and must not
touch LVGL or assume the Page is mounted. For both contexts, `payload` is valid
only during the callback; copy every byte that must survive it.

A Page must not retain a subscription while backgrounded. Work that must survive
App inactivity belongs to an independent service/system owner with its own
explicit stop policy; an App-private worker or session still must stop when the
App becomes backgrounded. The Page may acquire a copied snapshot on RESUME. START
and STOP may allocate or release passive instance state, but they are not a
second active-resource lifetime.

For `EVENT_BUS_DISPATCH_PUBLISHER`, `event_bus_unsubscribe()` only prevents new
callbacks: a callback already past its liveness check may still run. A
Page-owned publisher subscription whose `user_data` is Page state therefore needs
more than a plain unsubscribe. Either its callback must not access any storage
that teardown can release, or PAUSE must use `event_bus_unsubscribe_sync()` under
that API's task and deadline rules, retain the handle on timeout, and clear it only
after the drain succeeds. Do not rely on the arena surviving PAUSE: UNMOUNT and
STOP can follow immediately and free the instance.

For `EVENT_BUS_DISPATCH_UI`, call `event_bus_unsubscribe()` from the UI
dispatcher task. That serializes the cancellation and Page teardown with queued
callbacks, so the handle may be cleared after `ESP_OK` or `ESP_ERR_NOT_FOUND`.

## Resource Placement

| Resource | Default placement | Release point |
| --- | --- | --- |
| Small selection, copied argument, filter state | Page arena | Successful STOP or an explicit remount reset |
| Decoded file, network frame, large row set | Separately allocated owner | The stage that acquired it; released before a Page is backgrounded unless it is passive |
| Page worker, active session, timer, subscription or input | Page foreground runtime | PAUSE, after the Page can no longer produce side effects |
| Worker stack and IPC that must survive App inactivity | Independent service/system owner | Under that owner's stop policy, after worker exit is confirmed |
| Shared Page-family state | App-level owner | App/service exit, not one Page's PAUSE |
| Read-only table | Flash/`.rodata` | Firmware lifetime |

A grouping refactor, removal of static reservation and recovery of inactive heap
are different outcomes. Report size estimates with their allocation region and
ABI assumptions; do not claim a lower working peak or full shared-service
recovery without measurement.

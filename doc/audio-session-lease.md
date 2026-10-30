# audio_service Exclusive Audio Contract

## Scope and Ownership

`audio_service` provides singleton full-duplex PCM, an exclusive lease, and
system-level volume, mute, and PA request policy on top of the BSP audio device.
The BSP owns the device resources; the service has no worker, decoder, mixing, or
preemption. The public interface is in
`layers/middleware/components/audio_service/include/audio_service.h`.

- `audio_service_acquire(kind, &lease)` establishes the owner and starts DMA only
  on success. The handle remains `audio_service_lease_t { uint32_t token;
  uint32_t generation; }`; both fields must match.
- RECORD and MAINTENANCE may read and write; PLAYBACK is write-only. When busy,
  the call returns `ESP_ERR_INVALID_STATE`; it neither queues nor stops an
  existing consumer.
- `write_leased/read_leased` check initialization, two-field identity, RUNNING,
  admission, and permission inside the same mutex region, and register the
  in-flight I/O; the BSP is called outside the lock and the reference is returned
  afterwards. A stale handle therefore cannot enter a new stream across
  release/reacquire.
- An I/O rejected by the service zeroes the non-empty transferred output. An
  admitted BSP call may report partial progress on timeout or another error;
  consumers must continue from the untransferred tail and must not replay data
  already sent.
- Only the current owner may release. The owner is cleared only on success; a
  stale handle never touches DMA. A failed release (including a reservation
  conflict) does not mean the lease was released: the consumer must keep the
  same-generation handle and retry, and must not overwrite the cleanup obligation
  with a new acquire.
- Volume, mute, and PA are system request policy, not part of the lease; release
  does not reset them. The PA request value is not the physical level after DMA
  stops.

## Initialization, State, and Concurrency

The startup owner must serialize the first `audio_service_init()`, including
synchronization-object creation; board audio must be initialized first. After
initialization, the service isolates control transactions with an internal mutex
and the SUSPENDING reservation; consumers are not required to build their own
lifecycle lock.

| State | Current meaning |
| --- | --- |
| UNINITIALIZED | Cannot acquire or do I/O; a partial-init cleanup obligation may still require deinit |
| READY | DMA stopped; either idle without an owner, or the owner was retained across suspend |
| RUNNING | There is an owner and I/O is allowed for that generation |
| SUSPENDING | Exclusive reservation for drain/stop/commit; conflicting control returns INVALID_STATE |
| ERROR | Admission closed; either an owner is retained or the cleanup obligation is still open |

`configure` accepts only READY without an owner. `deinit` rejects an active lease;
the owner should release first. Deinit retains the synchronization objects and the
generation across deinit/init, so an old handle never revives on reinitialization.

Release, suspend, and deinit close admission and reserve SUSPENDING in the same
entry critical section. With no in-flight I/O they complete stop and the state
commit while holding the lock; only with in-flight I/O do they unlock to wait for
drained and then re-lock to stop/commit. A conflicting
acquire/configure/resume/release/suspend/deinit cannot enter that transaction,
clear the owner, or stop a new stream. A drain timeout restores the pre-entry
state and admission and keeps the handle; a stop failure moves to ERROR and keeps
the cleanup obligation. A release in READY with an owner clears the owner directly
without a second stop; a suspend in READY is a no-op with `resume_required=false`.

## ERROR and Retry

- Acquire is possible only when initialized and with no owner in READY/ERROR. In
  ERROR, stop cleanup runs first under the control lock; on failure the state
  stays ERROR and that error is returned — a new stream cannot start.
- A start failure in acquire is compensated immediately by stop in the same
  critical section. Successful compensation returns to READY, failure keeps ERROR;
  the original start error is returned, the output lease is zeroed, and no
  generation is allocated.
- A partial-init ERROR cannot acquire and is resolved by deinit/init; a failed
  deinit retains the information needed to retry.
- An ERROR with an owner may retry stop through a same-generation release, or
  retry start through resume. An arbitrary `INVALID_STATE` must not be normalized
  into a successful release.
- Resume returns OK without starting DMA when READY has no owner; it succeeds
  idempotently when RUNNING has an owner; it restarts with the original handle when
  READY/ERROR has an owner. Uninitialized, SUSPENDING, and ERROR with no owner
  return INVALID_STATE.

## Production Consumers and PM

- `main/app_runtime.c` supplies start-up PCM/volume/mute/PA policy and manages
  service initialization; it holds no playback lease.
- `recorder_service`: recording uses RECORD, WAV playback uses PLAYBACK. Leaving a
  page does not force-stop background recording or playback. A failed cleanup
  keeps the old handle, and a later start/play/deinit completes the old obligation
  first; the owner must not be overwritten.
- Flybirds `flybirds_audio_session.c` / `flybirds_audio.c`: use PLAYBACK; when the
  lease is unavailable the game stays silent and retries on its existing page
  cadence without interrupting a background consumer. The writer completion
  signal means no further access to application audio state, and the synth/PCM may
  be reused only after a successful reap; a failed release keeps the session for
  open/close to retry. Double mute still holds the lease and supplies 20 ms of
  PCM.
- `music_service`: online playback uses PLAYBACK. It configures the decoded
  source rate/channel/bit depth before acquire, restores the product default
  format after release, and keeps the lease independent of music App page
  lifecycle; playback continues with the screen off while the system PM guard
  blocks standby.
- `main/display_benchmark.c`: the audio maintenance load uses MAINTENANCE and must
  not reuse another owner's stream. Settings are modified only after a complete
  save; a failed restore keeps the snapshot and lease, and a failed release keeps
  the owner for stop to retry. Incomplete cleanup forbids a new benchmark from
  overwriting the context.
- `main/app_runtime_pm.c` is the actual suspend/resume caller. The system drains
  audio before sleep, the lease is retained across suspend, and resume after wake
  or a sleep rollback is called by PM according to the restore marker — not by the
  consumer alone. If the owner releases while suspended READY, a later PM resume
  is an idle OK and starts no ownerless DMA; if a new owner is already RUNNING,
  resume does not restart it.

## Verification Boundary

Service host tests cover lease identity, lifecycle, I/O admission, drain, and error
recovery; each consumer's host, integration, and simulator fakes must honor
same-generation validation, exclusivity, and owner retention on failure. Host and
simulator runs do not prove on-device I2S/DMA timing, audio quality, codec or
amplifier electrical behavior, or power consumption.

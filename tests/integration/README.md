# Cross-Layer Host Integration Tests

This suite links the production mailbox, builtin registry, lifecycle, event
bus, theme, shared UI, Setup Wi-Fi adapter, and product applications into host
executables. Files under `host/` replace only FreeRTOS,
LVGL, application metadata, and middleware service boundaries.

## Coverage

The two tests cover:

- `presentation`: transition effects, completion barriers, and fast-forward.
- `cross_layer`: Home -> Applications -> Clock/Recorder/Level -> Settings
  -> Setup -> Weather navigation, HOME switching, page pause/resume, optional
  service failures, and release of timers, subscriptions and page workers,
  global connectivity snapshots, one-shot system SNTP ownership, and
  page-owned RTC alarms, including trigger-time automatic disarm.
The cross-layer test also proves that latest-only Power and connectivity
snapshots preserve mailbox headroom. Setup queues a callback immediately
before exit, verifies teardown cancels it, reopens, rejects an old-generation
snapshot, and renders the current global snapshot. Closing Clock does not stop
the system-owned one-shot SNTP request.

Recorder commands are modeled as service-owned requests: the page submits
start/pause/resume/stop/play/delete and refreshes from a generation snapshot.
The production recorder worker owns PCM and filesystem I/O; `chore_service` is
reserved for short, bounded metadata or capacity work and is not used for the
audio loop.

## Run

From the repository root, run all three profiles:

```sh
cmake -S tests/integration -B /tmp/mt-cross-normal -G Ninja \
    -DCROSS_LAYER_SANITIZER=none
cmake --build /tmp/mt-cross-normal -j2
ctest --test-dir /tmp/mt-cross-normal --output-on-failure

cmake -S tests/integration -B /tmp/mt-cross-asan -G Ninja \
    -DCROSS_LAYER_SANITIZER=address
cmake --build /tmp/mt-cross-asan -j2
ctest --test-dir /tmp/mt-cross-asan --output-on-failure

cmake -S tests/integration -B /tmp/mt-cross-tsan -G Ninja \
    -DCROSS_LAYER_SANITIZER=thread
cmake --build /tmp/mt-cross-tsan -j2
ctest --test-dir /tmp/mt-cross-tsan --output-on-failure
```

These host checks do not replace ESP32-S3 validation for LVGL rendering,
driver timing, SD card insertion/removal, IMU and audio behavior, radio and RTC
behavior, screen-off/standby wake paths, or resource measurements.

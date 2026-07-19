# Cross-Layer Host Integration Tests

This suite links the production mailbox, builtin registry, lifecycle, event
bus, theme, shared UI, demo adapters, Setup Wi-Fi adapter, and all four
applications into host executables. Files under `host/` replace only FreeRTOS,
LVGL, application metadata, and middleware service boundaries.

## Coverage

The five tests cover:

- `presentation`: transition effects, completion barriers, and fast-forward.
- `cross_layer`: Home -> Demo Center -> Motion/Audio/Storage/Clock -> Settings
  -> Setup -> Home navigation, HOME switching, page pause/resume, optional
  service failures, and release of timers, subscriptions, workers, sessions,
  SNTP ownership, and page-owned RTC alarms, including trigger-time automatic
  disarm.
- `test_audio_demo_adapter`: low-amplitude PCM meter response, serialized
  commands, chunked tone cancellation, amplifier ownership, exact owner-side
  PSRAM worker deletion, and recovery after an audio write failure.
- `test_storage_demo_adapter`: exclusive temporary-file creation, preservation
  of colliding files, 4 KiB verification, partial-write cleanup, PSRAM
  worker-stack release, and retryable unlink failure.
- `test_clock_demo_adapter`: ten-second alarm configuration, protection of
  external alarms, owned SNTP/alarm release, PSRAM worker-stack release, and
  retryable resource cleanup.

The cross-layer test also proves that latest-only Power and Wi-Fi snapshots
preserve mailbox headroom. Setup queues a callback immediately before exit,
verifies teardown cancels it, reopens with a new session, rejects an old
session snapshot, and renders the current session snapshot.

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

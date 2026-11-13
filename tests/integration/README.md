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

This suite links app and app_manager sources, so a red build here is a
regression signal for any change under `layers/apps/` or `layers/app_manager/`
(see the impact table in [`doc/verification.md`](../../doc/verification.md)).
Run it with the `none` profile per task; the sanitizer profiles are only
required when a change touches memory ownership or concurrency.

From the repository root, run the normal profile:

```sh
cmake -S tests/integration -B /tmp/mt-cross-normal -G Ninja \
    -DCROSS_LAYER_SANITIZER=none
cmake --build /tmp/mt-cross-normal -j2
ctest --test-dir /tmp/mt-cross-normal --output-on-failure

```

The default fixture is the S3 reference product. To exercise the P4 product
composition, pass its matching target, board and product explicitly:

```sh
cmake -S tests/integration -B /tmp/mt-cross-p4 -G Ninja \
    -DMT_TARGET=esp32p4 \
    -DMT_BOARD=waveshare_esp32_p4_wifi6_touch_lcd_3_5 \
    -DMT_PRODUCT=p4_reference
cmake --build /tmp/mt-cross-p4 -j2
ctest --test-dir /tmp/mt-cross-p4 --output-on-failure
```

Use `-DMT_PRODUCT=minimal` with the default S3 target and board for the
minimal product fixture.

`CROSS_LAYER_SANITIZER=address` or `thread` is an additional profile for
memory-ownership or concurrency changes. Select the profile matching the risk
and use a separate build directory; both profiles are not required by default.

These host checks do not replace ESP32-S3 validation for LVGL rendering,
driver timing, SD card insertion/removal, IMU and audio behavior, radio and RTC
behavior, screen-off/standby wake paths, or resource measurements.

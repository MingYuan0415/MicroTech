# Cross-Layer Host Integration Tests

This suite links the production mailbox, builtin registry, lifecycle, event
bus, theme, shared UI, Setup Wi-Fi adapter, and all four applications into one
host executable. Files under `host/` replace only FreeRTOS, LVGL, clock, power,
screen, and Wi-Fi service boundaries.

## Coverage

The test exercises the real Home -> Menu -> Settings/Power -> Setup -> Home
navigation flow and verifies page-local event subscriptions are released. It
then proves that 1,000 power snapshots occupy one latest-only UI slot, while
1,000 Wi-Fi status and 1,000 scan snapshots occupy two slots and preserve
mailbox headroom. The Setup path also queues a callback immediately before
exit, verifies teardown cancels it, reopens with a new session, rejects an old
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
driver timing, radio behavior, or resource measurements.

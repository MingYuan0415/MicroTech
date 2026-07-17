# Connectivity Pipeline Host Test

This suite links the production WiFi worker, event bus, UI mailbox, and Setup
WiFi adapter. The host files replace only FreeRTOS, the mailbox timer, LVGL,
logging, and the ESP-IDF WiFi driver boundary.

It covers UI-worker callback identity, caller-owned credential lifetime,
scan and connection delivery, stale-session filtering, mailbox backpressure,
terminal snapshot retry, teardown callback suppression, and WiFi lifecycle
restart without recreating the process-lifetime worker.

Run from the repository root:

```sh
cmake -S tests/connectivity -B /tmp/mt-connectivity -G Ninja \
    -DCONNECTIVITY_SANITIZER=none
cmake --build /tmp/mt-connectivity
ctest --test-dir /tmp/mt-connectivity --output-on-failure
```

Use `address` for ASan/UBSan or `thread` for TSan.

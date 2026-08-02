# Connectivity Pipeline Host Test

This suite links the production connectivity manager, WiFi executor, event
bus, UI mailbox, and Setup WiFi adapter. The host files replace only FreeRTOS,
NVS, the mailbox timer, logging, and the ESP-IDF WiFi driver boundary.

It covers versioned profile validation, boot auto-connect, commit after IPv4,
failed replacement retention, storage failures, foreground priority, cancel,
forget, manual offline hold, the persistent auto-connect switch, long retry,
association and DHCP timeouts, stale epochs, suspend/resume, queue overflow,
credential zeroing, control timeout recovery, UI callback identity, and manager
lifecycle cleanup. A separate port test verifies that ESP-IDF WiFi init receives
`nvs_enable=true` before the driver is explicitly set to `WIFI_STORAGE_RAM`.

Run from the repository root:

```sh
cmake -S tests/connectivity -B /tmp/mt-connectivity -G Ninja \
    -DCONNECTIVITY_SANITIZER=none
cmake --build /tmp/mt-connectivity
ctest --test-dir /tmp/mt-connectivity --output-on-failure
```

Use `address` for ASan/UBSan or `thread` for TSan. These tests do not prove RF,
DHCP server, flash NVS, standby power, or cold-start behavior on hardware.

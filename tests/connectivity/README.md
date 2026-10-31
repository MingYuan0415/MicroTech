# Connectivity Pipeline Host Test

This suite links the production connectivity manager, Wi-Fi executor, event bus,
UI mailbox, and Setup Wi-Fi adapter. Host files replace only FreeRTOS, NVS, the
mailbox timer, logging, and the ESP-IDF Wi-Fi driver boundary.

It covers versioned profile validation and migration, bounded scan-match-connect
cycles, commit after IPv4, failed replacement retention, storage failures,
foreground priority, cancel, forget, Wi-Fi enable/disable, per-profile attempt
limits, scan target matching, association and DHCP timeouts, stale epochs,
suspend/resume, credential zeroing, control timeout recovery, UI callback
identity, and manager lifecycle cleanup.

The saved-network list is covered separately: SSID upsert without duplicates,
refusing a save when every slot is held, targeted connect and targeted removal,
removal of the active entry versus an inactive one, list survival across a full
manager restart, and `FORGET` clearing every entry. `wifi_cycle` adds the policy
invariants: only profiles present in the scan are attempted, each profile gets a
bounded number of attempts, failover reaches the next saved profile, an
exhausted cycle stops the radio and clears `wifi_enabled`, a scan can preempt a
connect attempt after its terminal, and the selected SSID/attempt ordinal stay
consistent through a failed sweep. It also covers NVS write failures during
enable/disable/forget and removal of the last profile.

A separate port test verifies that ESP-IDF Wi-Fi init receives `nvs_enable=true`
before the driver is explicitly set to `WIFI_STORAGE_RAM`, and that filtered
scans report presence for saved SSIDs that are not in the strongest five UI
records.

Run from the repository root:

```sh
cmake -S tests/connectivity -B /tmp/mt-connectivity -G Ninja \
    -DCONNECTIVITY_SANITIZER=none
cmake --build /tmp/mt-connectivity
ctest --test-dir /tmp/mt-connectivity --output-on-failure
```

Use `address` for ASan/UBSan or `thread` for TSan. These tests do not prove RF,
DHCP server, flash NVS, standby power, or cold-start behavior on hardware.

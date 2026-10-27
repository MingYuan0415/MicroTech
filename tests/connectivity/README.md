# Connectivity Pipeline Host Test

This suite links the production connectivity manager, WiFi executor, event
bus, UI mailbox, and Setup WiFi adapter. The host files replace only FreeRTOS,
NVS, the mailbox timer, logging, and the ESP-IDF WiFi driver boundary.

It covers versioned profile validation and migration, boot auto-connect, commit
after IPv4, failed replacement retention, storage failures, foreground priority,
cancel, forget, manual offline hold, the persistent auto-connect switch, long
retry, association and DHCP timeouts, stale epochs, suspend/resume, queue
overflow, credential zeroing, control timeout recovery, UI callback identity, and
manager lifecycle cleanup.

The saved-network list is covered separately: SSID upsert without duplicates,
refusing a save when every slot is held, targeted connect and targeted removal,
removal of the active entry versus an inactive one, list survival across a full
manager restart, `FORGET` clearing every entry, and a sweep that advances to the
next saved network on `AP_NOT_FOUND` and parks in the long-backoff policy once
every candidate has failed.

Four cases pin the invariants that are easy to break: a record written with more
slots than the build keeps loads as a clamped list with a valid selected index and
stays loadable after the rewrite; an exhausted sweep leaves the selected profile
and the persisted `active_index` on the user's choice; removing an entry that does
not carry the link keeps the link, the automatic-connect policy, and the radio out
of the manual-offline hold; and a refused removal write rolls the RAM list back so
it matches flash across a reboot.

A separate port test verifies that ESP-IDF WiFi init receives `nvs_enable=true`
before the driver is explicitly set to `WIFI_STORAGE_RAM`.

Run from the repository root:

```sh
cmake -S tests/connectivity -B /tmp/mt-connectivity -G Ninja \
    -DCONNECTIVITY_SANITIZER=none
cmake --build /tmp/mt-connectivity
ctest --test-dir /tmp/mt-connectivity --output-on-failure
```

Use `address` for ASan/UBSan or `thread` for TSan. These tests do not prove RF,
DHCP server, flash NVS, standby power, or cold-start behavior on hardware.

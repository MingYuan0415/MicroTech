# Configuration and Dependency Governance

This document describes how MicroTech assigns ownership of configuration items,
board facts, runtime policy, protocol constants, and external dependencies, and
where the machine checks live. The rules follow the current implementation; the
tests under `tests/` are the executable specification, and this document states
only what they already enforce.

## 1. Ownership Rules

| Category | Owner | Carrier |
| --- | --- | --- |
| Static resource budget, compile-time switches | Kconfig | each component's `Kconfig` / `Kconfig.projbuild` |
| Runtime policy (sample rate, time zone, SNTP, PCM format, mount paths, weather endpoints, etc.) | Caller | root `app_product_config_t` and its sub-configuration structs |
| Board facts (resolution, bus frequency, DMA geometry, pins, calibration) | Board backend | `layers/bsp/waveshare/.../board_display_profile.h` and the `board.cmake` manifest |
| Protocol constants and semantics | Contract submodule | `contracts/device_link` (`protocol.yaml`, `vectors/golden.json`) |
| External dependency versions | Manifest + lock | each `idf_component.yml` and the root `dependencies.lock` |

The LVGL and board profile gates in the root `CMakeLists.txt` (cache, allocator,
RGB565, NimBLE SC-only, TLS, PSRAM external BSS, and so on) are build
enforcement; keep them as they are — neither weakened nor strengthened.

## 2. Configuration Rules (Machine-checked)

The following rules are enforced by
`tests/configuration/test_configuration_governance.py`, which scans production
sources (`main/`, `layers/`, excluding `tests/`, `managed_components/`,
`XPowersLib/`, `build/`, `probe/`):

1. Compile-time fallbacks of the form `#ifndef CONFIG_*` are forbidden; a
   configuration item is always used at its build-time resolved value.
2. `configTICK_RATE_HZ` must not be redefined.
3. `.c`/`.h` files must not call `xTaskCreate`, `xTaskCreateStatic`, or
   `xTaskCreateWithCaps` without a pinned core; task core affinity is always
   explicit.
4. Removed configuration symbols must not reappear. The list is
   `DEPRECATED_CONFIG_TOKENS` in `test_configuration_governance.py` (it covers the
   deleted `CONFIG_BSP_AUDIO_*`, `CONFIG_BSP_DISPLAY_*`,
   `CONFIG_APP_MANAGER_DISPLAY_BENCHMARK`, and the old per-service task and queue
   symbols).
5. Removed configuration must not appear in `sdkconfig.defaults` or in
   `tests/display/profile_defaults/*.defaults`.
6. The project's Kconfig symbol set is not machine-pinned; adding or removing a
   symbol needs no test update, but a removed symbol must still be registered in
   `DEPRECATED_CONFIG_TOKENS` (rule 4) so it cannot come back.
7. The key connectivity, cache, and task-core baseline in `sdkconfig.defaults` is
   fixed; a change requires updating the expected mapping in
   `test_connectivity_defaults` in the same change.

Configuration changes land in `sdkconfig.defaults` (`idf.py save-defconfig`);
never edit `sdkconfig` by hand.

## 3. Dependency and Manifest Rules

Enforced by `tests/configuration/test_dependency_pins.py` (part of this
governance):

1. Registry dependency versions in every `idf_component.yml` must not be empty or
   `*` (the test checks the literal `""`/`"*"` per entry; bounded ranges are
   allowed).
2. A registry dependency name declared in `idf_component.yml` must exist in the
   root `dependencies.lock`; version consistency is guaranteed by the component
   manager resolving against the lock, the test does not compare versions entry by
   entry.
3. Every dependency in the root `requirements*.txt` must be pinned exactly
   (`name==version`); range specifiers are forbidden.

After changing a version in `idf_component.yml`, re-resolve and update
`dependencies.lock`; never edit the lock by hand.

## 4. Check Entry Point

```sh
python3 tests/run_python_tests.py configuration contracts
```

The `configuration` group covers the configuration rules, the removed-symbol
lists, the connectivity baseline, and dependency pins; the `contracts` group runs
the contract submodule's own protocol and vector tests (host implementation
agreement with the golden vectors is covered by
`layers/middleware/components/device_link/tests/host`). They are local and review
entry points; they add no CMake `FATAL_ERROR` and no CI gate.

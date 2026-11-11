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
| Board identity, physical panel/platform, system-PM, hosted-radio and radio-availability facts | Board backend | `layers/bsp/waveshare/.../board.cmake` and `board_display_profile.h` |
| Product App composition, service capabilities and product-only policy | Product manifest | `products/<product>/product.cmake`; panel/platform/PM/hosted-radio fields are inherited from the board |
| App sources, include dirs, services, capabilities and reset hooks | App manifest | `layers/apps/<app>/app.cmake`, listed by `app_catalog.cmake` |
| Protocol constants and semantics | Contract submodule | `contracts/device_link` (`protocol.yaml`, `vectors/golden.json`); the submodule pointer is the only code pin |
| Contract schema metadata | Middleware component | `layers/middleware/components/device_link/device-link-contract.lock` |
| External dependency versions | Manifest + lock | each `idf_component.yml` and the per-target `dependencies.lock.<target>` |

A product may add non-BSP service capabilities (for example radio), but the
selected board must declare `MT_BOARD_RADIO_SUPPORTED` for radio and it cannot
declare a peripheral capability the board backend has not implemented. The App
catalog is explicit; recursive globs are forbidden.

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
4. The key connectivity, cache, and task-core baseline in `sdkconfig.defaults`
   is fixed; a change requires updating the expected mapping in
   `test_connectivity_defaults` in the same change.
5. Product, App catalog and board contracts are checked by
   `test_product_catalog.py`: every App source is catalogued, every product
   selects known Apps and a known board, required Apps are selected, App
   capabilities are available on the product, board document registries are
   present and well formed, and each product design target matches the board
   display profile.

Configuration changes land in `sdkconfig.defaults` (`idf.py save-defconfig`);
never edit `sdkconfig` by hand.

## 3. Dependency and Manifest Rules

Enforced by `tests/configuration/test_dependency_pins.py` (part of this
governance):

1. Registry dependency versions in every `idf_component.yml` must not be empty or
   `*` (the test checks the literal `""`/`"*"` per entry; bounded ranges are
   allowed).
2. A registry dependency name declared in `idf_component.yml` must exist in the
   matching per-target `dependencies.lock.<target>`; version consistency is
   guaranteed by the component manager resolving against the lock, the test does
   not compare versions entry by entry.
3. Every dependency in the root `requirements*.txt` must be pinned exactly
   (`name==version`); range specifiers are forbidden.

After changing a version in `idf_component.yml`, re-resolve and update the lock
for each affected target; never edit a lock by hand.

The project keeps one lock per IDF target: `dependencies.lock.<target>`. The
root `CMakeLists.txt` sets the Component Manager's `DEPENDENCIES_LOCK` build
property so each target resolves into its own file; the files are committed and
are not a combined release manifest. `tests/configuration/test_dependency_pins.py`
judges conditional manifest entries against each lock's own `target`, so a
deliberate S3 or P4 build does not make the other target's lock look stale.
Build and verify each target separately from a clean build directory.

## 4. Check Entry Point

```sh
python3 tests/run_python_tests.py configuration contracts
```

The `configuration` group covers the configuration rules, the connectivity
baseline, and dependency pins; the `contracts` group runs
the contract submodule's own protocol and vector tests (host implementation
agreement with the golden vectors is covered by
`layers/middleware/components/device_link/tests/host`). They are local and review
entry points; they add no CMake `FATAL_ERROR` and no CI gate.

---
name: esp-idf
description: Practical ESP-IDF, FreeRTOS, LVGL, board, and hardware guidance for the MicroTech firmware project. Use when writing, reviewing, debugging, building, reconfiguring, flashing, or monitoring ESP-IDF applications and components, especially CMake component requirements, generated files, ESP_* error macros, platform state, and device drivers.
license: MIT
metadata:
  source: https://github.com/Sped0n/systems
  adapted-for: MicroTech (Waveshare ESP32-S3 Touch AMOLED 1.8)
---

# ESP-IDF

Use for ESP-IDF platform and application work in the MicroTech repository. Repository-level rules in `AGENTS.md` and `doc/code-style.md` take precedence over this skill.

## Scope

- Own ESP-IDF, FreeRTOS, LVGL, board/platform, peripherals, power, sensors, buses, provisioning, build, flash, and monitor work.
- Keep app hardware behavior outside dependency and vendor code. Do not edit `managed_components/`, ESP-IDF, or BSP third-party libraries (e.g. `XPowersLib`) unless explicitly asked.
- Prefer existing component and platform boundaries over new wrappers or layers.
- `layers/` directories are independent git submodules: changes there are committed in their own repositories, and the parent repository records pointer updates.

## Architecture and State

- Keep platform/hardware policy near its owner; UI renders snapshots and sends intents, not protocol or hardware decisions.
- Centralize mutation of app/platform state in one owner. Derive UI busy, error, and enabled states from its enum rather than parallel booleans.
- Keep timers, task/queue work, callbacks, retries, and cleanup for one lifecycle in one orchestration path. Serialize work unless concurrency is required.
- Transition before observable work begins; transition on asynchronous completion, not request submission. Preserve usable prior state after a failed refresh when safe.
- Do not let LVGL screens own driver state, and do not scatter GPIO or power policy through UI callbacks.

## Errors

- Follow the repository error-handling convention in `doc/code-style.md` section 6: use the `MT_ERROR_HANDLE` macro from `mt_log.h` with a single `exit` label for functions with cleanup obligations, cleaning up in reverse acquisition order.
- No-cleanup parameter validation and ordinary failures return directly; never introduce `goto` merely for a single return point.
- Validate inputs first, keep the happy path flat, and log at the boundary that adds context. Avoid noisy polling-path logs.
- Log via `mt_log.h` (`LOG_E`/`LOG_W`/`LOG_I`/`LOG_D`/`LOG_V` with `DBG_TAG` defined at the top of each `.c`); never redefine `DBG_TAG` in headers.

## Build and Device Workflow

- Run `idf.py build` from the project root that owns the top-level `CMakeLists.txt`; run the smallest relevant check first.
- Run `idf.py reconfigure` after source moves or CMake source-discovery changes. Do not repair generated Ninja or CMake state manually.
- `REQUIRES` and `PRIV_REQUIRES` must not depend on `CONFIG_xxx`; the component graph is expanded before configuration is loaded.
- Do not manually modify `sdkconfig`, `sdkconfig.old`, `dependencies.lock`, `managed_components/`, or build output unless explicitly requested. Config changes go into `sdkconfig.defaults` (via `idf.py save-defconfig`); report unexpected lockfile changes.
- After changing cache, DMA reservations, or resources, run `idf.py size` and verify display, touch, standby, and wake-up on hardware.
- Formatting: run the astyle dry-run and the relevant host test suites before committing (commands in `AGENTS.md` and `doc/code-style.md`).
- Flashing changes hardware state; do it only when requested or clearly required.
- For device failures, capture recent logs and inspect reset reason, panic, heap, task watchdog, startup placement, and app state transitions.

## Review

- One module owns each platform state and lifecycle.
- Driver, board, and power policy do not leak into UI or unrelated feature code.
- Component requirements are configuration-independent.
- Generated and managed files remain untouched unless in scope.
- Run `git diff --check`; run `idf.py build` for compile-affecting changes and state any hardware-validation gap.
- Host tests do not replace on-board verification: driver timing, RF, DMA, power, and resource usage must be checked on hardware.

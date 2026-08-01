---
name: esp-idf
description: MicroTech ESP-IDF engineering workflow for component dependencies, CMake and Kconfig, generated state, logging and error cleanup, build and reconfigure, basic monitoring, and explicitly authorized flash operations. Use when changing or reviewing ESP-IDF project structure, manifests, configuration, build behavior, or device workflow; use esp32 or lvgl-integration for domain facts, debug-esp32s3 for live-device diagnosis, and validate-firmware for validation scope and acceptance evidence.
---

# ESP-IDF Workflow

Apply this workflow to ESP-IDF engineering in the MicroTech repository. Follow
`AGENTS.md` and `doc/code-style.md` first; inspect the current checkout before
using remembered commands, versions, or component behavior.

## Skill Routing

- Use `esp32` for board pins, buses, electrical constraints, and peripheral
  facts. Use `lvgl-integration` for the display pipeline and benchmark facts.
- Use `debug-esp32s3` when a live-device failure needs serial, ELF, JTAG, GDB,
  core-dump, watchdog, heap, task, USB, or permission evidence.
- Use `validate-firmware` to select tests, sanitizers, build and size checks,
  hardware procedures, and the acceptance claim supported by their results.

## Repository Boundaries

- Treat `layers/bsp`, `layers/middleware`, `layers/app_manager`, and
  `layers/apps` as independent Git submodules. Commit a changed submodule
  before updating the root gitlink.
- Keep `managed_components/`, ESP-IDF, build output, and BSP third-party code
  read-only. Prefer dependency upgrades or project-owned wrappers.
- Prefer existing component ownership and public APIs over new cross-layer
  wrappers or dependencies.

## Components and Configuration

- Keep `REQUIRES` and `PRIV_REQUIRES` independent of `CONFIG_xxx`; ESP-IDF
  expands the component dependency graph before loading project configuration.
- Run `idf.py reconfigure` after moving sources or changing CMake source
  discovery. Never repair generated Ninja or CMake state by hand.
- Change component requirements in the owning `idf_component.yml`. Let the
  Component Manager regenerate `dependencies.lock`, then inspect and report
  the resulting lockfile change; never hand-edit the lockfile.
- Do not hand-edit `sdkconfig` or `sdkconfig.old`. Persist configuration in
  `sdkconfig.defaults` using `idf.py save-defconfig`, and do not weaken the
  profile checks in the root `CMakeLists.txt`.

## Errors and Logging

- Validate parameters before work. Return ordinary failures directly when no
  cleanup, unlock, rollback, or centralized logging obligation exists.
- Route ordered failure paths with cleanup obligations to one `exit` label,
  clean up in reverse acquisition order, and preserve the first business
  error. Use `MT_ERROR_HANDLE` only where its contract in `doc/code-style.md`
  applies.
- Log through `mt_log.h`. Define `DBG_TAG` and `DBG_LVL` in each `.c`, add
  context at the owning boundary, and avoid duplicate or high-frequency logs.

## Build and Device Workflow

- Let `validate-firmware` choose the smallest relevant host, sanitizer, build,
  size, and hardware matrix for the requested claim.
- For startup failure, panic, or hang, preserve recent logs before changing
  code, then use `debug-esp32s3` for the evidence ladder.
- Run flash or erase operations only after the user explicitly authorizes the
  hardware state change. Monitoring and read-only diagnostics do not imply
  authorization to flash.

## Completion Checks

- Use `validate-firmware` to execute and report proportionate checks. Inspect
  generated or lockfile changes and every affected submodule separately.

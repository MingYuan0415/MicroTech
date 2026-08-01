---
name: lvgl-integration
description: MicroTech LVGL 9.5 display-pipeline integration and debugging for esp_lvgl_adapter 0.6.2, the 368 x 448 SH8601A QSPI AMOLED panel, FT5x06-compatible touch, PSRAM draw buffers, DMA staging, display lifecycle, transitions, and on-board performance. Use for blank or corrupt display, tearing, touch mapping, flush completion, tick scheduling, color format, buffer ownership, display memory, or benchmark facts; use debug-esp32s3 for live crash evidence and validate-firmware for validation scope.
---

# MicroTech LVGL Display Pipeline

Apply this skill to the existing MicroTech display architecture. Read current
code and locked dependencies instead of introducing a generic LVGL port or
copying API catalogs into the repository.

## Sources of Truth

- Read `main/idf_component.yml` and `dependencies.lock` for the active adapter
  and LVGL versions.
- Read `layers/app_manager/app_core/src/app_manager_lvgl_adapter.c` for display
  registration, custom draw submission, diagnostics, input, and lifecycle
  ownership.
- Read
  `layers/bsp/waveshare/esp32-s3-touch-amoled-1.8/board_display.c` for the
  SH8601A panel bus, touch path, power sequencing, and exported display port.
- Read `layers/bsp/Kconfig.projbuild`, `layers/app_manager/app_core/Kconfig`,
  and `tests/display/profile_defaults/` for display defaults and benchmark
  overrides. Read `sdkconfig.defaults`, the root `CMakeLists.txt`, and
  `tests/display/README.md` for the enforced platform profile and empirical
  acceptance limits.
- Verify LVGL API names against the installed `managed_components/lvgl__lvgl`
  headers and adapter behavior against
  `managed_components/espressif__esp_lvgl_adapter`; keep both directories
  read-only.

## Ownership Contracts

- Let `esp_lvgl_adapter` own LVGL initialization, tick/task scheduling,
  display registration, and the normal flush-ready handshake.
- Let App Manager own adapter configuration, the panel proxy and custom
  draw-bitmap callback, display diagnostics, transitions, input consumption,
  and suspend/resume coordination.
- Let the BSP own panel IO, touch IO, QSPI, GPIO, panel power, reset sequencing,
  and physical display state.
- Do not add `esp_lvgl_port`, a second LVGL task or tick source, a parallel
  display registration path, or direct screen ownership of panel hardware
  unless the user explicitly approves replacing the architecture.
- In the custom draw callback, submit through `esp_lcd_panel_draw_bitmap` and
  return its `esp_err_t`. Do not also call flush-ready: the adapter handles
  asynchronous completion and its failure fallback.
- Use LVGL 9 names such as `lv_display_flush_ready` when inspecting the
  handshake. Do not write new code against deprecated v8 aliases.

## Diagnostic Order

1. Capture the current `display config` and failure logs. Confirm the locked
   LVGL and adapter versions before reasoning about APIs.
2. Confirm that App Manager, the adapter, and the BSP retain their ownership
   boundaries and that display suspend/resume drains in-flight work.
3. Prove a solid-color flush through the existing adapter path before
   investigating widgets. Check submission results, completion notification,
   and the adapter-owned `lv_display_flush_ready` handshake exactly once.
4. Check resolution, stride, RGB565 versus RGB565-swapped, draw-buffer rows,
   DMA chunking, queue depth, PSRAM capabilities, cache behavior, and the
   internal DMA reserve together.
5. Check the adapter-managed tick/task schedule and UI worker progress before
   changing application timers.
6. Validate FT5x06-compatible touch interrupt flow and coordinate transforms
   independently of display rendering.
7. Measure with the on-board display benchmark; do not infer driver timing,
   tearing, DMA stability, or power behavior from host tests.

## Project Baseline

- Resolve the production baseline from the current BSP and App Manager Kconfig
  defaults, and benchmark overrides from `tests/display/profile_defaults/`.
  In the current checkout these yield 40 MHz QSPI, 60-row LVGL partial
  buffers, 10-row SPI DMA chunks, RGB565, TE disabled, and non-TE PSRAM direct
  DMA disabled. Do not add redundant copies to `sdkconfig.defaults`.
- Treat 80 MHz, TE synchronization, and direct PSRAM DMA as explicit
  characterization paths, not production fixes. Require the evidence and
  acceptance gates in `tests/display/README.md` before changing defaults.
- After changing cache, buffer, DMA reserve, assets, or fonts, run
  `idf.py size`, the relevant App Manager and integration host tests, a full
  firmware build, and scoped on-board validation.

## Completion Evidence

- Report locked versions, resolution, color format, buffer and DMA geometry,
  queue/direct/TE settings, and the observed flush ownership path.
- Report benchmark TARGET/FLOOR results and any snapshot fallback, panic,
  watchdog, OOM, freeze, tearing, or artifact.
- Distinguish host coverage, one reset boot, manual effect checks, cold power
  cycles, and long-duration stress; never claim beyond the executed scope.
- Use `debug-esp32s3` when a display symptom includes panic, watchdog, OOM,
  core-dump, JTAG, task, USB, or serial-tool evidence. Use
  `validate-firmware` to select and report the complete validation matrix;
  keep display-specific acceptance facts in this skill and the display README.

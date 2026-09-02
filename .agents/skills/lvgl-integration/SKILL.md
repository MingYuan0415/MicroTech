---
name: lvgl-integration
description: MicroTech display-pipeline ownership boundaries and hard rules for esp_lvgl_adapter, App Manager, and the BSP panel/touch path on the 368 x 448 SH8601A QSPI AMOLED panel. Use before changing display registration, draw submission, flush handshake, suspend/resume, or transitions; use esp32 for panel pins, buses, and power, and layers/bsp/README.md plus tests/display/README.md for baselines and benchmark acceptance.
---

# MicroTech LVGL Display Pipeline

This skill carries only the ownership rules that no single file states. Baseline
values, benchmark gates, and pin maps live in the owning docs and code; do not
copy them here.

## Sources of Truth

- `dependencies.lock` for the active adapter and LVGL versions.
- `layers/app_manager/app_core/src/app_manager_lvgl_adapter.c` for display
  registration, custom draw submission, diagnostics, and lifecycle ownership.
- `layers/bsp/waveshare/esp32-s3-touch-amoled-1.8/board_display.c` for panel
  bus, touch path, and power sequencing.
- `layers/bsp/README.md` and `tests/display/README.md` for the production
  baseline (clocks, buffer rows, DMA chunks, queue depth, TE/direct-DMA) and
  benchmark acceptance gates.

## Ownership

- `esp_lvgl_adapter` owns LVGL init, tick/task scheduling, display
  registration, and the flush-ready handshake.
- App Manager owns adapter configuration, the panel proxy and custom
  draw-bitmap callback, diagnostics, transitions, input consumption, and
  suspend/resume coordination.
- BSP owns panel IO, touch IO, QSPI, GPIO, panel power, and reset sequencing.

## Hard Rules

- Do not introduce `esp_lvgl_port`, a second LVGL task or tick source, a
  parallel display registration path, or app-level ownership of panel hardware
  unless the user explicitly approves replacing the architecture.
- In the custom draw callback submit through `esp_lcd_panel_draw_bitmap` and
  return its `esp_err_t`; never call `lv_display_flush_ready` there — the
  adapter owns asynchronous completion and its failure fallback.
- Write new code against LVGL 9 names only; do not revive v8 aliases.
- Suspend/resume must drain in-flight display work. Treat 80 MHz, TE
  synchronization, and direct PSRAM DMA as characterization paths, not
  production fixes; changing defaults requires the evidence gates in
  `tests/display/README.md`.
- Validation scope follows the `AGENTS.md` default workflow; do not expand it
  from this skill.

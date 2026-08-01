---
name: lvgl-integration
description: Use when integrating, porting, configuring, or debugging LVGL, display drivers, input devices, ticks, draw buffers, or embedded GUI performance on the MicroTech AMOLED display pipeline.
license: MIT
metadata:
  source: https://github.com/easyzoom/aix-skills
  adapted-for: MicroTech (LVGL 9.5, RGB565, 40 MHz QSPI)
---

# LVGL Integration

Use this skill to integrate LVGL by proving four porting contracts: display flush, input device read, tick/timebase, and memory/draw buffer configuration. UI bugs often come from driver timing or buffer ownership, not widget logic.

## When To Use

Use this skill when:

- The user wants to add or debug LVGL on the MicroTech AMOLED pipeline.
- The issue involves blank screen, wrong colors, tearing, touch offset, slow refresh, memory faults, tick handling, or display flush callbacks.
- The project has display, touch, encoder, keypad, RTOS, or DMA/cache constraints.

## First Questions

Ask for:

- LVGL version (this project: 9.5) and target platform (ESP32-S3).
- Display controller (SH8601A), resolution (368 x 448), color format (RGB565), interface (40 MHz QSPI), and framebuffer/draw buffer strategy (60-row PSRAM draw buffer, 10-row SPI DMA chunk, bounce DMA).
- Input devices: touch, buttons, or none.
- Tick source and `lv_timer_handler` scheduling.
- RAM budget, heap policy, RTOS use, DMA/cache involvement.
- Current symptom and minimal screen test result.

## Integration Checklist

1. Bring up display flush.
   Fill the screen with solid colors before creating complex widgets.

1. Configure draw buffers.
   Buffer size, color format, stride, and cache/DMA policy must match the display path.

1. Provide a reliable tick.
   LVGL needs a monotonic tick and regular handler execution.

1. Add input after display.
   Touch should be calibrated and tested independently.

1. Bound memory.
   Configure heap, widget count, image assets, fonts, and buffers for the target RAM.

1. Verify refresh performance.
   Measure frame time, flush completion, and whether `lv_disp_flush_ready` is called correctly.

## Common Failures

- Blank screen because flush callback never calls ready.
- Wrong colors from RGB/BGR or 16-bit endian mismatch (`RGB565` vs `RGB565_SWAPPED`).
- Touch coordinates need rotation/calibration transform.
- UI freezes because `lv_timer_handler` is not called regularly.
- DMA reads stale cache lines or writes to non-DMA-capable memory (use the internal 128 KiB reserve / `MALLOC_CAP_DMA`).
- Fonts/images exceed flash or RAM budgets.

## Verification

Before claiming LVGL works:

- State LVGL version, resolution, color format, buffer size, and tick period.
- Confirm solid color flush, a label/button screen, and input event if applicable.
- Confirm `lv_timer_handler` schedule and flush-ready behavior.
- Confirm memory usage and DMA/cache policy when relevant.

For the MicroTech performance and stability acceptance criteria, use the on-board benchmark described in `tests/display/README.md` (`display benchmark`/`display load` logs, TARGET/FLOOR thresholds). Display behavior changes must be validated on hardware; host tests do not cover driver timing.

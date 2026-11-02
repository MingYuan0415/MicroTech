---
name: esp32
description: Check MicroTech BSP hardware changes against the supported Waveshare ESP32-S3 Touch AMOLED 1.8 and ESP32-P4-WIFI6-Touch-LCD-3.5 topologies, including pins, strapping, buses, memory, power, radio ownership, and wake capabilities. Use for board facts and physical behavior, not generic ESP32-family advice.
---

# MicroTech ESP32 Hardware

Apply this skill to the two boards supported by MicroTech. The ESP32-S3
sections below are the original topology; the ESP32-P4 section at the end
covers the second variant. Do not transfer pin rules between the boards or from
the original ESP32, WROOM, WROVER, or another Waveshare board.

## Sources of Truth

- Read `AGENTS.md` and `layers/bsp/README.md` first.
- Read the concrete implementation under
  `layers/bsp/waveshare/esp32-s3-touch-amoled-1.8/` or
  `layers/bsp/waveshare/esp32-p4-wifi6-touch-lcd-3.5/` before describing pins,
  buses, reset sequencing, or lifecycle behavior.
- Read `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3` /
  `sdkconfig.defaults.esp32p4`, and the root `CMakeLists.txt` before describing
  flash, PSRAM, cache, DMA reserve, or CPU settings.
- Use the official Waveshare [Resources and Documents](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.8/Resources-And-Documents)
  page as the discovery index for the board schematic, Espressif ESP32-S3
  datasheet and TRM, and the linked SH8601A, FT3168, QMI8658C, PCF85063A,
  AXP2101, and ES8311 documents. Select only the document relevant to the
  subsystem under review and verify its model and revision.
- Check the board schematic for electrical or pin-repurpose work, and the
  current ESP-IDF ESP32-S3 capability headers for SoC limits. Treat local PDF
  copies as caches and compare them with the current official link when a
  decision depends on document revision; do not depend on machine-specific
  paths or infer facts from another ESP32 or peripheral variant.
- Keep ESP-IDF, `managed_components/`, and `layers/bsp/XPowersLib/` read-only.

## Fixed Board Identity

- Target the Waveshare ESP32-S3 Touch AMOLED 1.8 with 16 MB flash and Octal
  PSRAM.
- Treat the 368 x 448 SH8601A QSPI panel, FT5x06-compatible touch path,
  TCA9554 expander, PCF85063 RTC, AXP2101 PMU, QMI8658C IMU, ES8311/NS4150B
  audio path, and removable SDSPI storage as the supported board topology.
- Use the BSP capability and lifecycle APIs. Do not let applications bypass
  the BSP to own GPIO, bus, power, or peripheral-driver state.

## PSRAM Stacks and Flash Access

- `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` permits PSRAM-backed task
  stacks. The `lvgl` worker uses one (`stack_in_psram = true` in
  `app_manager_lvgl_adapter.c`); the display-diagnostics task is another when
  enabled, and any task created with `MALLOC_CAP_SPIRAM` is subject to the
  same rule.
- A flash write, erase, or commit disables the flash and PSRAM caches. If the
  active stack pointer is in PSRAM, ESP-IDF asserts in
  `spi_flash_disable_interrupts_caches_and_other_cpu()`; without the assert the
  stack would be inaccessible while the cache is off.
- Never perform an NVS commit or erase (`nv_storage_*` or `nvs_*` writes,
  `nvs_erase_*`, `nvs_commit`), an `esp_partition_*` write or erase, an OTA
  flash operation, or a flash-backed filesystem write from the LVGL worker, an
  LVGL event/timer/draw callback, or any other PSRAM-stacked task. Queue small
  storage work on `chore_service` (internal-RAM worker) or the owning service
  worker, and keep new storage workers on an internal-RAM stack (the default
  `xTaskCreate*` path, or `MALLOC_CAP_INTERNAL` with the caps APIs).
- NVS and flash reads through the cache are allowed; the restriction is on
  operations that disable the cache.
- Do not move the LVGL worker stack to internal RAM to make a storage call
  legal. `tests/display/README.md` requires the render task stack in PSRAM;
  move the storage call off the worker instead.

## ESP32-S3 Pin Guardrails

- Treat GPIO0, GPIO3, GPIO45, and GPIO46 as ESP32-S3 strapping pins. Verify
  their reset-time levels and the board schematic before changing connected
  circuitry, while recognizing that the board legitimately uses them after
  boot.
- Do not apply the original-ESP32 GPIO12 flash-voltage rule. On this board,
  GPIO12 is the LCD chip-select signal.
- Do not mark GPIO6 through GPIO11 as original-ESP32 flash pins. The board
  legitimately uses GPIO4 through GPIO7 for QSPI data and GPIO11 for the LCD
  clock.
- Do not treat GPIO34 through GPIO39 as input-only on ESP32-S3. Confirm usable
  input and output masks in the current ESP-IDF ESP32-S3 `soc_caps.h`.
  Capability does not imply board availability: keep GPIO26 through GPIO32
  reserved for SPI flash/PSRAM and GPIO33 through GPIO37 reserved by the Octal
  memory interface unless the exact module and schematic prove otherwise.
- Preserve the current LCD QSPI mapping from `board_display.c`: clock GPIO11,
  chip select GPIO12, data GPIO4/5/6/7, and TE GPIO13. Preserve shared I2C
  SDA GPIO15, SCL GPIO14, and touch interrupt GPIO21 unless a schematic-backed
  board revision explicitly changes them.

## Hardware Workflow

1. Identify the owning BSP subsystem and its current pin, bus, and lifecycle
   definitions.
2. Check shared buses, strapping behavior, expander routing, power rails,
   wake-source capability, and PSRAM/DMA allocation requirements.
3. Compare any electrical or pin change with the board schematic and current
   ESP32-S3 datasheet or ESP-IDF capability definitions.
4. Keep ISRs short and IRAM-safe where required. Keep blocking I2C, expander,
   and peripheral work in task context.
5. Select tests by actual source and contract impact under `AGENTS.md`.
   Driver timing, DMA, power, sleep/wake, radio coexistence, and physical
   behavior need hardware evidence; report unexecuted checks as limitations,
   not as implicit authorization or a requirement to flash on every task.

## ESP32-P4 Board (second variant)

- Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, ESP32-P4NRW32X (in-package
  32 MB HEX PSRAM, 16 MB external NOR flash), 2 HP RISC-V cores.
- The connected chip is ESP32-P4 rev v1.3 (pre-3.0 silicon). Keep
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESP32P4_REV_MIN_100=y` and the
  360 MHz CPU ceiling. Rev <3.0 and >=3.0 images are mutually exclusive, and a
  rev >=3.0 image is rejected by this chip at flash time. `esp_audio_codec`
  must stay below 2.6 for this revision; `main` pins the 2.5 line for P4.
- Display: ST7796 320 x 480 RGB565 over 4-wire SPI: MOSI GPIO20, SCLK GPIO21,
  CS GPIO23, DC GPIO26, RST GPIO27, backlight GPIO28. No TE. The BSP keeps the
  panel hidden until the first `resume_commit`; the panel IO splits strips into
  20-row DMA chunks with queue depth 2.
- Touch: FT6336 (FT5x06 compatible) on I2C SCL GPIO8 / SDA GPIO7 at 400 kHz,
  RST GPIO29, INT GPIO50.
- Key input: HOME GPIO35 and POWER GPIO49, both active low. GPIO35 is a boot
  strapping pin (GPIO35-38); keep its pull-up and do not add a large capacitor.
- Power: AXP2101 on the shared I2C (address 0x34). Its IRQ is not routed to the
  P4, so poll the status registers over I2C. Do not reprogram the default power
  rails; the board's AXP defaults power the SoC. The PWR key also drives the
  AXP PWRON input.
- Audio: ES8311 on the shared I2C with MCLK GPIO13, SCLK GPIO12, DOUT GPIO9,
  LRCK GPIO10 and DIN GPIO11. DOUT carries playback into the codec's DSDIN and
  DIN carries the codec's ASDOUT capture; do not swap them. NS4150B PA enable is
  GPIO53 (active high, held off until `bsp_audio_start()`). The capture PGA runs
  at 36 dB.
- Storage: microSD on SDMMC slot 0, 4-bit, CLK GPIO43, CMD GPIO44,
  D0-D3 GPIO39-42. The slot's IO rail is supplied by the on-chip LDO4, which the
  BSP acquires before mounting. There is no card-detect pin; mounting is
  explicit and formatting is never automatic.
- No IMU, no RTC chip, no TCA9554, no TE pin. The BSP does not register RTC or
  IMU capabilities.
- C6 coprocessor: SDIO CLK GPIO18, CMD GPIO19, D0-D3 GPIO14-17, reset GPIO54,
  C6_IO2 strap GPIO6. The hosted stack owns these pins and the C6 reset; the
  BSP must not drive them. C6 UART and IO9 are on header J6 (23/25/27, GND 29).
- Power management: RTCIO is GPIO0-15; both keys are outside it and the P4 has
  no GPIO light-sleep wake source. CPU light sleep cannot be woken by the keys,
  so `board_get_wakeup_descriptor()` is empty and `app_runtime` skips
  `system_pm_init`. Only the app-manager screen-off path is available.
- The P4 radio is hosted: `CONFIG_ESP_HOSTED_HOST_FEAT_WIFI` and
  `..._FEAT_BT` route `esp_wifi_*` and NimBLE HCI to the C6. Do not add native
  `CONFIG_ESP_WIFI_*`/`BT_CTRL_*` controller settings or
  `CONFIG_ESP_HOST_WIFI_ENABLED`.

## Display Boundary

Use `lvgl-integration` for LVGL adapter ownership, draw buffers, flush
handshakes, transitions, and display performance. Use this skill for the BSP
panel bus, GPIO, touch, power sequencing, and hardware constraints beneath
that pipeline.

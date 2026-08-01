---
name: esp32
description: MicroTech ESP32-S3 and Waveshare ESP32-S3 Touch AMOLED 1.8 board-hardware guidance. Use for BSP drivers, GPIO assignments and strapping, I2C/SPI/I2S buses, Octal PSRAM and DMA capabilities, RTC/PMU/IMU/audio/SD hardware, sleep and wake sources, electrical constraints, or physical board behavior; use debug-esp32s3 for live-device evidence and validate-firmware for acceptance scope. Do not use as a generic ESP32-family reference.
---

# MicroTech ESP32-S3 Hardware

Apply this skill only to the ESP32-S3 board supported by MicroTech. Do not
transfer pin rules from the original ESP32, WROOM, WROVER, or another
Waveshare board.

## Sources of Truth

- Read `AGENTS.md` and `layers/bsp/README.md` first.
- Read the concrete implementation under
  `layers/bsp/waveshare/esp32-s3-touch-amoled-1.8/` before describing pins,
  buses, reset sequencing, or lifecycle behavior.
- Read `sdkconfig.defaults` and the root `CMakeLists.txt` before describing
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
5. Run the owning BSP host tests and cross-layer tests for behavior changes.
6. Validate driver timing, DMA, power, sleep/wake, radio coexistence, and
   physical peripheral behavior on hardware; report the exact tested scope.

Use `validate-firmware` to choose and report that test matrix. Use
`debug-esp32s3` for serial, ELF, core-dump, OpenOCD, JTAG, GDB, task, watchdog,
heap, USB, port, and permission evidence from a failing device.

## Display Boundary

Use `lvgl-integration` for LVGL adapter ownership, draw buffers, flush
handshakes, transitions, and display performance. Use this skill for the BSP
panel bus, GPIO, touch, power sequencing, and hardware constraints beneath
that pipeline.

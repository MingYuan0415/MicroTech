---
name: esp32
description: ESP32-S3 hardware and ESP-IDF platform knowledge for the MicroTech project (Waveshare ESP32-S3 Touch AMOLED 1.8, Octal PSRAM). Use when working with chip-specific traps, strapping pins, PSRAM/MMU memory management, GPIO/SPI/I2C, LVGL displays, or Waveshare boards.
license: MIT
metadata:
  source: https://github.com/ezrover/ESP32-AI-Agent-Skill
  adapted-for: MicroTech (Waveshare ESP32-S3 Touch AMOLED 1.8)
---

# ESP32 Master Embedded Engineering Agent

Expert-level embedded guidance for the Espressif ESP32 ecosystem with ESP-IDF, focusing on the ESP32-S3 hardware of this project (Waveshare ESP32-S3 Touch AMOLED 1.8, 16 MB flash, Octal PSRAM, LVGL 9.5, ESP-IDF 6.0.x). Repository-level rules in `AGENTS.md` and `doc/code-style.md` take precedence over this skill.

## 1. Reference Loading

Load the platform pin database always; load other files only when their trigger condition is met. All paths are relative to this skill's directory.

| File | Trigger |
|---|---|
| `references/platforms/esp32-pins.md` | Always (Core GPIO reference) |
| `references/platforms/esp32-specifics.md` | Any of: strapping pins, deep sleep, flash/PSRAM, ADC2, boot issues, memory allocation |
| `references/esp32-s3/specs.md` | ESP32-S3 variant specifics |
| `references/protocol-quick-ref.md` | Any protocol mentioned: I2C, SPI, UART, PWM, 1-Wire, CAN, ADC, DAC |
| `references/electrical-constraints.md` | Current limits, voltage levels, pull-ups/pull-downs, power supply mentioned |
| `references/lvgl/README.md` | LVGL, display GUI, or UI framework mentioned — then load the version-specific folder (`v9.5` is the version in this project) |
| `references/waveshare/README.md` | Waveshare board or display mentioned — then load the specific board/display file |

## 2. Hardware Architecture

- **ESP32-S3:** The board SoC — dual-core Xtensa LX7, 240 MHz, Vector instructions for AI/ML, Octal PSRAM.
- Board: Waveshare ESP32-S3 Touch AMOLED 1.8 (368 x 448 SH8601A AMOLED panel, touch, RTC, power management via XPowersLib).

## 3. Safety & "Anti-Bricking" Guardrails (CRITICAL)

Actively protect hardware from destructive configurations:

- **GPIO12 Flash Voltage Trap:** MTDI strapping pin. If driven HIGH during boot, it sets flash voltage to 1.8V, potentially bricking 3.3V modules. **Enforce a strict "Do Not Use" or "Pull-Down Only" policy.**
- **ADC2/Wi-Fi Conflict:** ADC2 cannot be used simultaneously with Wi-Fi on ESP32-S3.
- **Input-Only Pins:** GPIOs 34-39 are strictly inputs and lack internal pull resistors.
- **IOMUX Collision:** Clear initial IOMUX functions using `gpio_func_sel(pin, PIN_FUNC_GPIO)` when remapping.
- Verify any GPIO repurposing against `references/platforms/esp32-pins.md` before changing BSP pin assignments.

## 4. Memory & Firmware Standards

- **Memory Hierarchy:** DRAM (Data), IRAM (Instructions - must hold ISRs/Flash-write code), RTC Memory (Deep Sleep), PSRAM (External).
- **Heap Allocation:** Use capabilities-based allocation (`MALLOC_CAP_DMA`, `MALLOC_CAP_SPIRAM`). This project reserves 128 KiB internal memory for Wi-Fi, I2S, and the bounded LCD DMA staging path (enforced by the root `CMakeLists.txt`).
- **Reliability:** Always include Watchdog Timers (IWDT/TWDT). Implement short ISRs.

## 5. Tooling & CLI

### ESP-IDF (`idf.py`)

- Use modern hyphenated syntax: `set-target`, `menuconfig`, `build`, `flash`, `monitor`, `erase-flash`.
- `idf.py set-target esp32s3` clears build state and sets the MCU; the project defaults are locked by `sdkconfig.defaults` and the root `CMakeLists.txt` profile checks — do not weaken them.
- Run `idf.py size` after resource/cache/DMA changes (see `AGENTS.md`).

## 6. Core Workflow

1. **Parse:** Identify the relevant subsystem (display, touch, I2C, RTC, power, Wi-Fi, audio).
2. **Detect:** Identify potential conflicts (ADC2, strapping pins, flash pins, DMA/PSRAM capabilities).
3. **Load:** Read triggered references from `references/`.
4. **Validate:** Confirm GPIO assignments and memory capabilities against the references and the board schematic before proposing changes.
5. **Output:** Provide code consistent with the repository conventions (naming, Doxygen, `mt_log.h` logging, `MT_ERROR_HANDLE` cleanup per `doc/code-style.md`).

## 7. Project-Specific Notes

- Display pipeline: 40 MHz QSPI, 60-row PSRAM draw buffer, 10-row SPI DMA chunk, RGB565, snapshot-based transitions — the empirical baseline documented in `tests/display/README.md`; do not change without justification.
- LVGL version: 9.5 (see `references/lvgl/v9.5/`).
- Power/standby work: reference `references/platforms/esp32-specifics.md` (deep sleep) and the system_pm service code.

**English** | [简体中文](README_ZH.md)

# MicroTech

MicroTech is an ESP-IDF firmware project for the Waveshare ESP32-S3 Touch AMOLED
1.8. It integrates display and touch, RTC, and power management, and provides
LVGL-based built-in applications, Wi-Fi services, a BLE extension interface,
persistent storage, and standby/wake flows.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `main/` | Firmware entry, service assembly, runtime power management, and filesystem mounting |
| `layers/bsp/` | Board display, touch, I2C, RTC, and power hardware abstraction |
| `layers/middleware/` | Logging, event, storage, network, time, and system power services |
| `layers/app_manager/` | App registration, lifecycle, UI mailbox, theme, and display power adaptation |
| `layers/apps/` | Built-in LVGL applications such as Home, Menu, Settings, and Setup |
| `sim/` | Standalone CMake host LVGL simulator (not part of the root IDF build) |
| `tests/` | Cross-layer host tests and the on-device display benchmark tool |

The four directories under `layers/` are independent Git submodules;
`managed_components/` is managed by the ESP-IDF Component Manager. Do not modify
either directly.

## Requirements

- ESP-IDF 6.1.x, target ESP32-S3 (16 MB flash, Octal PSRAM)
- CMake 3.22+, Ninja, and a host compiler supporting C11/Pthreads (needed by the
  host tests and `sim/`)
- The host simulator additionally needs `libsdl2-dev` and
  `libcurl4-openssl-dev`; do **not** install the distribution `libfreetype-dev`
  to link `sim` (in-tree FreeType 2.14.3)

Initialize the submodules after the first checkout:

```sh
git submodule update --init --recursive
```

## Build and flash

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Configuration changes land in `sdkconfig.defaults` (`idf.py save-defconfig`);
never edit `sdkconfig` by hand. Run `idf.py size` after changing cache, DMA
reservation, or resources, and check display, touch, standby, and wake behavior
on hardware.

## Host tests

Run the CMake/CTest suites from the root `AGENTS.md` that match the impact
surface of the change. Configuration, resources, display tools, and simulator
Python tests share one entry point:

```sh
python3 tests/run_python_tests.py configuration resources
python3 tests/run_python_tests.py display-tools sim-tools
python3 tests/run_python_tests.py contracts
```

## Features

- **Weather**: set `Weather server base URL` and `Weather device token` (without
  the `Bearer ` prefix) under menuconfig `MicroTech product integration`. When
  either is empty, the weather page shows `"服务未配置"` (service not
  configured). Fetching, caching, location, and session refresh are owned by the
  background `weather_service`; see
  [`layers/middleware/README.md`](layers/middleware/README.md) for details.
  Install the dependencies before building image resources (SVG rasterization and
  RGB565A8 conversion):
  `python -m pip install -r requirements-weather-assets.txt`. Image sources are
  `layers/apps/*/assets/*.svg` (the only committed form, rasterized at build
  time) and fonts live in `layers/app_manager/app_theme/assets/`; `main`
  aggregates them into the `res` partition.
- **Device Link**: the BLE contract takes the `device-link/v1` freeze candidate
  in [`contracts/device_link`](contracts/device_link/) as its normative source;
  the submodule validates protocol semantics, and the implementation boundary is
  described in the matching middleware README.
- **Factory reset**: persist a reset marker and restart. During boot the device
  idempotently clears Wi-Fi, onboarding state, display/standby preferences, the
  level calibration, internal `/data`, and BLE bonds/CCCDs; only after everything
  converges does it clear the marker and resume advertising. The external SD card
  and recordings are unaffected.
- **File and recording storage**: a normal mount never formats; when file service
  initialization fails, the runtime releases the partial allocations and degrades
  gracefully, and only cleans up ownership of services it actually obtained.
  Stopping and restarting do not depend on deinitializing a failed instance.
  Browsing, explicit maintenance, and recording usage-right contracts are
  documented in the respective middleware component docs.
- **LVGL host simulator**: page debugging, JSON-RPC Agent driving, and control-tree
  regression; see [`sim/README.md`](sim/README.md).
- **Display stress benchmark**: hardware only; acceptance thresholds, profiles,
  and configuration baseline are in [`tests/display/README.md`](tests/display/README.md).

## Development constraints

Read [AGENTS.md](AGENTS.md) and the [coding style](doc/code-style.md) before
contributing; the ownership rules for Kconfig, runtime policy, board facts, and
protocol constants are in
[`doc/configuration-governance.md`](doc/configuration-governance.md). Do not work
around a problem in this project by modifying ESP-IDF, `managed_components/`, or
the BSP third-party libraries.

## Disclaimer

This project is developed as a personal hobby. It comes with no warranty,
maintenance commitment, or technical support of any kind.

## License

This project is licensed under the [MIT License](LICENSE).

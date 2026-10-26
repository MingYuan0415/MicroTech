# Display Performance and Stability Acceptance

This directory holds the host tools for the ESP32-S3 display baseline. The
baseline is for development firmware only; production configuration disables
`CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS` and `CONFIG_MAIN_DISPLAY_BENCHMARK`.
After the benchmark starts, the root runtime suppresses standby and releases
the suppression when the run ends or startup rolls back.

## Fixed Display Baseline

The board profile for the Waveshare ESP32-S3 Touch AMOLED 1.8 is fixed at:

- 368x448, QSPI 40 MHz, 4 data lines, RGB565;
- 10-line logical transfer, 44-line DMA full-line limit, transaction queue
  depth 2;
- PSRAM Direct DMA off, TE off.

These values come from the BSP private profile and the read-only transport
descriptor; they do not enter Kconfig or the benchmark defaults. LVGL's 60-line
dual PSRAM draw buffer, Snapshot, task stacks, diagnostics gate, and core
affinity remain cross-board static resources or build capabilities.

Default malloc is fixed to PSRAM first
(`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`). LVGL objects, style nodes, and the
FreeType glyph cache therefore do not occupy internal memory; the 128 KiB
`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` is reserved for LCD/Wi-Fi/I2S DMA
staging, task stacks, and cache-disabled buffers. Components that need internal
memory must explicitly use `heap_caps_malloc(MALLOC_CAP_INTERNAL)` or
`MALLOC_CAP_DMA`. The FreeType per-size cache is fixed at
`CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=64`, which caps the combined PSRAM peak
of the six font sizes.

Historical characterization showed that 80 MHz improved the worst P95 by about
3.45%, short of the 5% decision line, and produced insufficient DMA contiguous
blocks and seams during motion; non-TE PSRAM Direct DMA produced a blue line at
the top and GUI freeze once TCP load began. Neither is therefore an executable
profile for the current board, and no clock, Direct DMA, or RGB swapped A/B
build entry point is retained. 40 MHz is still the project experience baseline
and does not represent an SH8601A preliminary timing guarantee.

## Benchmark Profiles

`tests/display/profile_defaults/benchmark_baseline.defaults` fixes the 60-line
draw buffer, Snapshot, and benchmark gate. Mode, duration, load, and network
parameters are selected by `tests/display/profiles/*.json`, and the tools
generate `display_benchmark_profile.h` into each isolated build directory; when
the gate is enabled but `DISPLAY_BENCHMARK_PROFILE_DIR` is not passed, CMake
rejects the build outright.

- `STRESS`: runs production transitions plus the Audio/TCP/BLE/App load
  selected by the profile.
- `CHARACTERIZATION`: runs fade, push-left, push-right, cover-left, and
  reveal-right each display-only and each with the specified load window.

The final report separates stability from performance:

- `stability=PASS`: completes fully with no sampling, display submission,
  Audio, TCP, or Wi-Fi errors, and a DMA/internal maximum contiguous block of
  at least 14,720 B.
- `performance=TARGET`: active average at least 30 FPS, at least 95% of frame
  intervals no longer than 33.333 ms, P99 no longer than 41.667 ms, and no two
  consecutive long frames.
- `performance=FLOOR`: active average at least 25 FPS, with P95/P99/max no
  longer than 50/66.667/100 ms respectively.

`display config` records `transport=qspi`, `bus_hz`, DMA geometry, queue,
Direct/TE, and the LVGL configuration from the BSP descriptor. `dma_fail` means
the DMA/internal maximum contiguous block was below the threshold at sampling
time; actual display submission failures are counted by `submit_fail`.

## TCP Echo Service

Run on a host address reachable from the device:

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

The device connects to `192.168.0.205:5001` by default; the address, port, and
per-direction rate come from the strict JSON profile. WSL mirrored networking
mode requires allowing TCP 5001 in the Hyper-V firewall.

## Snapshot Transitions

The default RGB565 configuration enables
`CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION`. For a valid non-`NONE`
transition, the App Manager captures the source and the target as two
full-resolution RGB565 PSRAM images. At 368x448 each is 329,728 B, and the two
resident buffers total 659,456 B, without occupying internal/DMA heap.

Snapshot depends only on SPIRAM and its own build gate. If buffer allocation,
capture, overlay, or animation start fails, it falls back to native LVGL Screen
animation; `NONE`, zero duration, the same Screen, and edge-back `PRESSING` do
not create a snapshot. Hardware acceptance requires no fallback for any valid
effect and prepare P95 no longer than 100 ms.

## Current Display Campaign

The tools only prepare the production C_EXT configuration and select the load
through a runtime campaign:

| Campaign | Load |
| --- | --- |
| characterization | both loads, five transitions |
| stress | 1800 seconds of production transitions |
| ble-stress | 1800 seconds of BLE connected combined load |
| soak | 28800 seconds of combined load |

Prepare, build, and validate the isolated candidates:

```sh
python3 tests/display/display_profiles.py prepare --reset --campaigns characterization ble-stress
# run the idf.py build size commands printed by the previous step in order
python3 tests/display/display_profiles.py validate --campaigns characterization ble-stress
python3 tests/display/analyze_characterization.py /path/to/characterization.log
```

A source change requires a fresh `prepare --reset`. The analyzer directly
verifies the raw byte counts, sample counts, cleanup, route/effect masks,
Snapshot, DMA, render task, and performance thresholds emitted by the device.

## C_EXT and Retained Stress

`C_EXT` is the production default: LVGL uses `LV_OS_NONE`, a single draw unit,
and a 32 KiB PSRAM adapter worker pinned to CPU1; project tasks and the
configurable major ESP-IDF system tasks are pinned to CPU0; NimBLE dynamic
memory uses PSRAM. The original C profile is retained as a historical baseline.
IDLE, IPC, driver-internal tasks, and ISRs are not covered by the project task
affinity guarantee.

```sh
python3 tests/display/display_profiles.py prepare --reset \
  --campaigns stress ble-stress
# run the generated isolated build commands in order
python3 tests/display/display_profiles.py validate \
  --campaigns stress ble-stress
```

`C_EXT_STRESS` adds the Device Link ACL connection load on top of `C_EXT`
(`ble_connected` mode, verifying that the binding window opens, the phone
connects, and disconnect/reconnect is jitter-free); it is retained for later
stability reproduction and does not modify the production `sdkconfig.defaults`.
The device runs Device Link ACL, Wi-Fi/TCP, Audio, Microphone, display
transition, and App Manager routing loads; Device Link v1 encrypted traffic
acceptance comes back after hardware Numeric Comparison verification. Log
analysis command:

```sh
python3 tests/display/analyze_c_ext_stress.py /path/to/c_ext_stress.log
```

The analyzer checks phase order, heap/DMA, task HWM/stack placement and core,
BLE connection retention, TCP, Audio TX/RX, the 13 animations, page coverage,
and cleanup recovery. A single 1800-second result cannot replace the 10-minute
pre-check, BLE toggle, TCP interruption recovery, cold boot, sleep resume, or
8-hour soak.

## Lifecycle Trace

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` controls INFO traces for START, MOUNT,
RESUME, PAUSE, UNMOUNT, STOP, and NEW_INTENT. It is off by default in
production; errors, warnings, handlers, observers, and state transitions are
unaffected.

## Reproducible Evidence Bundle

A comparable acceptance run must archive the following as a group; none may be
missing:

- **display config**: the `display config transport=... bus_hz=...` line printed
  by the firmware at boot in the device log, with fields from the BSP descriptor
  and the LVGL configuration; the current analyzers read it by the
  `display config ` marker.
- **profile files**: `tests/display/profiles/*.json` (strict schema, see
  `display_profile_utils.load_benchmark_profile`) plus the
  `tests/display/profile_defaults/*.defaults` layered in at build time; archive
  together the `display_benchmark_profile.h` and `source_manifest.json` that
  `display_profiles.py prepare` generates in the isolated directory.
- **firmware source fingerprint**: the `source_fingerprint` and
  `repositories[*].commit` that `prepare` writes into `source_manifest.json`,
  covering the commits, diffs, and untracked file contents of the parent
  repository and the submodules.
- **raw log**: the complete device serial log, given to
  `analyze_c_ext_stress.py <log>` or `analyze_characterization.py <log>`.

The tools in this directory do not produce **Device ID** or **Boot ID** fields,
and the parsers do not read them; the tools also do not compute a
**firmware image hash**. The log only contains IDF boot markers
(`boot: ESP-IDF`, `rst:0x`), used to count restarts rather than to identify a
single device or a single boot. To distinguish devices or boots you must record
it yourself and cannot claim it was produced by these tools.

A run missing any of the above items must not be compared against any baseline;
if `source_fingerprint` or the profile differs, that run is not grouped with the
old baseline.

## Debug Order

1. First verify the adapter-managed `lv_display_flush_ready` handshake with a
   solid-color flush.
2. Check the resolution, RGB565, stride, 60-line draw buffer, 10-line DMA
   staging, and queue depth 2.
3. Check the adapter tick, the single `lv_timer_handler` worker, and UI
   progress.
4. Verify touch coordinates and rotation independently, then add the UI on top.
5. Check widgets, fonts, images, Snapshot, and the PSRAM/internal/DMA budget.
6. Use the on-device benchmark to judge frame time, flush completion, fallback,
   and the contiguous-block threshold.

Host tests can only verify configuration, lifecycle, and parsing logic; they
cannot prove SPI timing, DMA, RF, power consumption, or real display stability.

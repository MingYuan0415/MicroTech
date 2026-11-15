---
name: device-operations
description: Debug and operate a connected MicroTech ESP32-S3 or ESP32-P4 board with mt.py device. Use for serial logs, boot identity, panic/coredump triage, explicit flashing, and hardware smoke checks; not for simulator work or multi-device ownership.
---

# Device Operations

Use this skill for a single locally connected board. The repository owns the
build and verification contracts; this skill only describes the device loop.

## Sources of Truth

- `doc/verification.md` owns the verification tiers and device-fault order.
- `README.md` owns the build/flash command and board selection.
- `mt.py build idf --coredump` applies `sdkconfig.coredump.defaults` locally
  and writes its generated config to `build/sdkconfig.coredump`; the committed
  `partitions_16m_single_app_res_data.csv` owns the partition.
- `esp-idf-monitor`, `esp-coredump`, and `esptool` behavior comes from the
  installed ESP-IDF tools; `mt.py doctor` reports their availability.

## Commands

```sh
python3 tools/agent/mt.py build idf --coredump
python3 tools/agent/mt.py device status [--port P] [--probe] [--json]
python3 tools/agent/mt.py device logs --snapshot --seconds 5 [--port P] [--reset]
python3 tools/agent/mt.py device logs --follow [--port P]
python3 tools/agent/mt.py device monitor --expect 'boot id=' --timeout 20 [--port P] [--reset]
python3 tools/agent/mt.py device crash [--port P] [--save-core PATH]
python3 tools/agent/mt.py device flash --port P --yes [--monitor]
python3 tools/agent/mt.py device smoke [--port P]
```

Use `device status` first. It reads the local build identity without touching
the board. `--probe` is explicit because `esptool` resets the chip to read the
MAC/chip ID. If several serial ports exist, pass `--port`; do not guess.

## Triage Order

1. `device status`: target, product, app version, ELF SHA-256, coredump partition.
2. `device logs --snapshot --reset`: boot id, reset reason, panic backtrace.
3. If the boot banner reports a crash reset, run `device crash`; do not reflash
   before the coredump has been read.
4. Check heap headroom, task WDT, and partition/boot placement in that order.
5. Only after the cause is understood, consider an explicit `device flash`.

The boot line contains `boot id=<hex> reason=<reason> version=<version>
elf=<sha256 prefix> partition=<label>`. The `elf=` value is the configured
short SHA-256 prefix, so compare it as a prefix of the full `ELF file SHA256`
from `device status`. A new boot id is expected after a reset; the same boot id
with no new log line means the monitor was attached after boot.

## Flashing Safety

- Flash only when the user explicitly asks. `device flash` is locked behind
  `--yes`.
- Never run `erase-flash`, erase NVS, or format the SD card as part of a
  debugging loop unless the user separately asks.
- The command compares the pre/post MAC when `--probe` succeeds; a changed MAC
  is a failure, not a normal board swap.
- `device smoke` resets the board and checks for the boot banner; it does not
  write flash.

## Boundaries

- Do not add device ownership, takeover, multi-project leases, OTA, or a
  network gateway for this single-board workflow.
- The simulator is the right tool for layout and interaction iteration;
  `device` is only for hardware-specific behavior (radio, audio, display
  timing, PMIC/RTC, real coredumps).
- For a P4 board, remember the hosted C6 radio image is a separate build; the
  host app can boot and degrade to offline UI when that link is unavailable.

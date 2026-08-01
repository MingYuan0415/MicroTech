---
name: debug-esp32s3
description: MicroTech ESP32-S3 live-device diagnostic workflow for startup failures, panic and watchdog resets, hangs, OOM, core dumps, ELF backtraces, serial or USB discovery, port and permission failures, OpenOCD, JTAG, GDB, and FreeRTOS task inspection. Use when gathering or interpreting evidence from a failing device; use esp-idf for project configuration, esp32 for board facts, lvgl-integration for display-pipeline causes, and validate-firmware for acceptance scope.
---

# Debug MicroTech ESP32-S3

Diagnose the current device without destroying the evidence that distinguishes
environment, tool, firmware, and hardware failures. Follow `AGENTS.md` first
and inspect the active checkout and installed ESP-IDF toolchain before using
remembered commands or paths.

## Sources of Truth

- Read the current logs, matching build ELF and map, partition table, and
  relevant implementation before forming a cause.
- From that same build, inspect its effective `sdkconfig`,
  `<build-dir>/config/sdkconfig.json`, `<build-dir>/project_description.json`,
  and any generated profile headers. Treat `sdkconfig.defaults` as an intended
  baseline, not as proof of the configuration in the flashed image.
- Discover serial devices through the current system and stable by-id paths.
  Treat `/dev/ttyACM0`, USB IDs, GDB ports, and OpenOCD configuration names as
  observations, not permanent board constants.
- Read the OpenOCD scripts, target GDB, and capability files from the installed
  ESP-IDF environment. Do not substitute ARM, original-ESP32, or another
  board's debugger instructions.
- Use `esp-idf` for build and configuration workflow, `esp32` for pins and
  electrical constraints, and `lvgl-integration` for display ownership.

## Evidence Preservation

- Capture the most recent output before resetting, attaching a debugger, or
  changing firmware. Prefer a monitor mode that does not reset the target.
- Record the exact firmware identity and matching ELF. Do not decode addresses
  against a stale or different build.
- Treat debugger attach, halt, and monitor connections as potentially
  intrusive. State any reset, task stop, USB disconnect, or timing distortion
  caused by the diagnostic method.
- Do not flash, erase, write memory or registers, change eFuses, or alter
  configuration without explicit authorization. Permission to inspect a
  device is not permission to change its state.

## Diagnostic Ladder

1. Record the symptom, expected behavior, reproduction boundary, firmware
   identity, connection method, and whether the evidence predates a reset.
2. Check device enumeration, stable serial path, access permissions, port
   ownership, cable or hub state, and availability of the matching ESP-IDF
   tools. Classify these failures as environment evidence until disproved.
3. Capture timestamped logs. Inspect reset reason, panic backtrace, heap and
   allocation failures, task or interrupt watchdog, startup placement, and
   application state transitions in that order.
4. Decode the complete backtrace with the matching ELF and the installed
   ESP32-S3 toolchain. Preserve raw addresses and decoder output together.
5. Compare repeated failures. Separate stable code locations from symptoms
   that move with boot timing, power, peripheral readiness, or workload.
6. Use OpenOCD and Xtensa GDB only when logs cannot answer the question.
   Confirm the current ESP32-S3 target configuration, halt as briefly as
   possible, and inspect threads, task stacks, registers, and backtraces.
7. Before editing code, state the leading hypothesis, contradictory evidence,
   and the cheapest check that can distinguish it from the next alternative.

## Failure Boundaries

- Report missing devices, access denial, a busy port, unsupported OpenOCD
  configuration, GDB connection failure, or ELF mismatch as tool or
  environment failures. Do not convert them into firmware conclusions.
- A ROM-idle core, exited task, blocked task, or low stack watermark needs
  application and scheduler context; it is not a root cause by itself.
- A panic in display work may require `lvgl-integration`; an I2C, GPIO, power,
  or peripheral hypothesis may require `esp32`. Keep the captured evidence in
  this workflow while routing factual ownership to the domain skill.
- Monitoring, one reset boot, one successful attach, and one successful
  reproduction prove only those events. Use `validate-firmware` to define the
  evidence required after a fix.

## Diagnostic Report

Report the symptom and firmware identity, collection method and intrusiveness,
raw evidence, decoded location, failure classification, ranked hypotheses,
next discriminating check, and any state-changing action that was not
authorized or not run.

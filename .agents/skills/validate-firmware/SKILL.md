---
name: validate-firmware
description: MicroTech firmware validation planning, execution, and evidence reporting across focused host tests, CTest suites, sanitizers, AStyle, ESP-IDF reconfigure and build, size checks, on-board behavior, display benchmarks, reset and cold-start cycles, and stress or soak testing. Use when selecting regression scope, validating a change before commit or release, deciding whether acceptance passed, or reporting unexecuted gaps; use the domain skills for implementation facts.
---

# Validate MicroTech Firmware

Select the smallest validation matrix that can support the requested claim,
then report exactly what ran. Follow `AGENTS.md` and the current test projects,
configuration, and topic documentation instead of copying commands or
thresholds into this skill.

## Sources of Truth

- Read `AGENTS.md`, `doc/code-style.md`, the owning test `CMakeLists.txt`, and
  its README before choosing commands or sanitizer options.
- Read `sdkconfig.defaults`, the root `CMakeLists.txt`, manifests, and
  `dependencies.lock` for build and configuration changes.
- Read `tests/display/README.md` and the current typed profile inputs for
  display benchmark modes, baselines, evidence fields, and acceptance gates.
- Inspect every affected submodule independently. Test and report its worktree
  before treating the root gitlink as the complete change.

## Classify the Change

- For documentation or skill-only changes, validate structure, links, focused
  text invariants, and `git diff --check`. Do not require a firmware build.
- For local pure logic, run the smallest owning host target, then the owning
  host suite. Add address or thread sanitizers when memory ownership,
  lifecycle, callbacks, synchronization, or concurrency changed.
- For cross-layer state or asynchronous behavior, run the affected layer
  suites plus connectivity or integration coverage and the relevant
  sanitizer variants.
- For CMake, Kconfig, source discovery, manifests, or locked dependencies,
  follow `esp-idf` for reconfigure and build behavior and inspect generated or
  lockfile changes without editing generated state.
- For cache, DMA reserve, buffers, partitions, packaged resources, fonts, or
  assets, add a firmware build and size comparison, then validate the affected
  resource behavior on hardware.
- For BSP drivers, timing, RF, storage, audio, power, sleep or wake, touch, and
  physical peripherals, combine host regressions with a full build and scoped
  on-board checks. Host results cannot prove hardware behavior.
- For display configuration or performance, let `lvgl-integration` supply the
  pipeline facts and `tests/display/README.md` supply the current benchmark
  profile and acceptance gates.

## Execute Safely

1. Define the exact claim: compile correctness, logic regression, one-device
   behavior, cold-start reliability, performance characterization, or release
   acceptance.
2. Map changed files and ownership boundaries to the smallest relevant tests.
   Include root and submodule worktrees without absorbing unrelated changes.
3. Run fast focused checks before broad suites, then sanitizers, build and size
   checks, and finally hardware validation when required.
4. Request explicit authorization before flash, erase, power cycling, sleep or
   wake manipulation, destructive storage tests, or other hardware state
   changes.
5. Preserve raw logs and profile identity for hardware runs. Record reset
   reason, panic, watchdog, OOM, restart, freeze, artifact, fallback, and
   peripheral errors relevant to the claim.
6. Stop at the verified boundary. A manual check, one reset boot, cold-cycle
   sample, characterization run, stress run, and soak run are distinct forms
   of evidence and are not interchangeable.

## Evidence Ledger

Report:

- change scope and requested acceptance claim;
- source documents, configuration, build identity, hardware and profile;
- commands or procedures executed and their results;
- resource or performance deltas when applicable;
- checks not run, why they were not run, and the risk that remains;
- authorized hardware mutations and the final device state.

Use `passed` only for the scope actually executed and `failed` when an executed
check misses its acceptance criteria. Use `not run` when a check was not
executed. Use `blocked` only after an attempted check cannot proceed because of
a concrete tool, environment, permission, device, or prerequisite failure;
name that blocker instead of treating an expected result as observed.

## Completion

Run `git diff --check`, inspect status in every affected repository, and keep
unrelated user changes outside the validation claim. Use `esp-idf`, `esp32`,
`lvgl-integration`, or `debug-esp32s3` when the result requires facts owned by
those skills.

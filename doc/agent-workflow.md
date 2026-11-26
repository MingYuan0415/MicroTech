# Agent Workflow

`tools/agent/mt.py` is the command-line entry point for agent-facing build,
test, formatting, Git, simulator, page-review, environment-check, and
device-debug output. It is a presentation layer: the wrapped command decides
pass or fail. What to run and when is defined in
[`doc/verification.md`](verification.md); this document covers command behavior
and output.

## Why

A full simulator or IDF build prints thousands of progress lines, and CTest
prints two lines per passing test. Those lines remain in the session history and
are paid again on every later request. The wrapper keeps the complete output on
disk and prints a bounded summary with a log path.

## Commands

```sh
python3 tools/agent/mt.py list
python3 tools/agent/mt.py build apps
python3 tools/agent/mt.py build sim
python3 tools/agent/mt.py test apps
python3 tools/agent/mt.py test integration
python3 tools/agent/mt.py test integration --no-build
python3 tools/agent/mt.py test weather-service --profile address
python3 tools/agent/mt.py py configuration resources docs
python3 tools/agent/mt.py py design-skills docs skill-tools
python3 tools/agent/mt.py fmt --check
python3 tools/agent/mt.py git status
python3 tools/agent/mt.py git diff
python3 tools/agent/mt.py git diff --patch -- <paths>
python3 tools/agent/mt.py sim --scenario agent_contract
python3 tools/agent/mt.py sim --no-build --scenario all_apps
python3 tools/agent/mt.py sim --jobs 4 --no-build
python3 tools/agent/mt.py ui --pages settings/wifi
python3 tools/agent/mt.py ui --pages settings/wifi --state scan_truncated --no-build
python3 tools/agent/mt.py doctor --json
python3 tools/agent/mt.py inspect --last
python3 tools/agent/mt.py build idf --coredump
python3 tools/agent/mt.py device status
python3 tools/agent/mt.py device logs --snapshot --seconds 10
python3 tools/agent/mt.py device monitor --expect 'boot id=' --timeout 20
python3 tools/agent/mt.py device crash
python3 tools/agent/mt.py device smoke
```

`build` accepts a suite alias, `idf`, or `sim`. `build sim` is the mandatory
simulator build gate defined in [`doc/verification.md`](verification.md): it
configures every canonical simulator product and derives each configuration's
panel geometry from its product's board manifest. `build idf --coredump`
optionally applies `sdkconfig.coredump.defaults` for crash capture; it is not
part of the committed baseline and keeps its generated configuration in
`build/sdkconfig.coredump` so a later plain build is unaffected. `test` builds
first, then runs CTest. `test --no-build` reuses the existing suite build and
runs CTest only; it requires a prior `build` or `test` for the same suite and
profile.
`--profile none|address|thread` selects the sanitizer profile supported by that
suite; each profile uses a separate build directory.

`sim --no-build` reuses `build/sim` after verifying its configured product
fixture, resources, config header, and `build-context.json`; use it for repeated
scenario runs.
Set `SIM_PRODUCT` when reusing a non-default product build. `sim --jobs N`
runs up to eight scenarios concurrently and `sim --keep-going` continues after
read-only assertion failures for diagnosis, stopping before the next
side-effect step; with `--jobs`, a hard failure prevents queued
scenarios from starting while already-running scenarios finish. These are
development-loop options: the default `mt.py sim` still performs the full gate,
and `mt.py build sim` remains the L3 acceptance command. `ui --state` restricts
a page review to named states for targeted debugging; a full page review is
still required before acceptance.

## Suites

Host suites are discovered from their `CMakeLists.txt`:

- `main`, `bsp`, `apps`, `app-core`, `integration`, `connectivity`;
- one alias per `layers/middleware/components/*/tests/host`, using the component
  directory name with `_` replaced by `-` (for example `weather-service`).

A suite's sanitizer option is read from its `CMakeLists.txt`; do not guess the
name. `mt.py list` shows the alias and option. `connectivity` needs `IDF_PATH`.

Python groups are selected separately with `mt.py py`; `design-skills` covers
the design-contract, aesthetic-review and motion-interaction helper tests,
while `skill-tools` covers the LVGL layout audit helper. `sim-build` runs the
simulator build gate and is included in `all`; the remaining groups are test
entry points only.

Build directories live under `build/agent/host/<alias>/<profile>`. The simulator
gate uses `build/sim-gate/<configuration>`; `build/sim` and `build/sim-real`
remain the development and page-review builds. Incremental artifacts survive
across agent turns. `MT_BUILD_ROOT` overrides the host build root.

## Output Contract

Successful commands print one `[PASS]` line, optional warning counts, and the
log path. Failures print `[FAIL]` or `[TIMEOUT]`, the extracted diagnostics, and
the log path. Summaries are capped at 40 lines / 4 KiB. A compiler line is
capped at 2 KiB; the complete line stays in the log.

When a failing command repeats the same normalized failure for the same command
and suite, the summary also prints `[LOOP] same failure xN for <command>
<suite>; previous: <timestamp> <log>; step back and try a materially different
hypothesis` after the headline. `N` is the consecutive count. The hint is
advisory; it does not retry, cache, or change the wrapped command's result or
exit code.

Exit codes:

- `0`: command passed;
- `1`: the wrapped command failed, or no tests were found;
- `2`: environment or usage error (missing `IDF_PATH`, submodule, tool, or
  dependency);
- `124`: timeout;
- `130`: interrupted.

`--full` prints the complete log. Pass the flag explicitly; there is no
environment override.

## Logs and Statistics

Complete output is written to `.cache/agent/logs/` (ignored by Git). Logs are
created with mode `0600`; keep them local and do not paste them into commits or
pull requests. `stats.jsonl` records output byte counts, duration, the exit
code, and on failure `failure_signature`, `failure_label`, and `repeat_count`;
passing entries store `null` signature/label and `0` repeats.

```sh
python3 tools/agent/mt.py log --last
python3 tools/agent/mt.py log --last --grep 'error:' --tail 20
python3 tools/agent/mt.py stats
python3 tools/agent/mt.py stats --failures
python3 tools/agent/mt.py stats --failures --json
python3 tools/agent/mt.py stats --failures --since 2026-11-01
```

`stats --failures` groups failures by command, suite, and normalized failure
signature. Entries written before signatures existed are grouped separately and
labeled `(legacy: no signature)`; `--since YYYY-MM-DD` filters by entry date.
The report itself returns `0` even when it lists failures, so use the grouped
rows rather than its exit code.

When a failure is not clear from the summary, read the log with `mt.py log` or
`rg -n -C2 'error:|FAILED' <log>`; do not rerun the gate just to see output.
After a repeated failure, read the latest retained log, verify the cause, and
write one verified lesson to the owning document or skill. Do not create a
generated fact cache or copy unverified guesses into versioned documentation.

## Never Compress

Use `--full` for:

- sanitizer and TSan reports;
- AStyle, whitespace, and line-ending review;
- security review;
- final acceptance evidence;
- logs supplied by the user or the device.

`git diff` defaults to `--stat`. Use `--patch` only for the files under review;
formatting review must use the raw patch.

## Simulator and UI Review

`sim` runs `sim/ci/run_ci.sh` with retained logs and reports. `--scenario NAME`
runs one scenario; omit it for all scenarios. The default build is quiet, but
compiler errors still appear.

`ui` builds the simulator, then runs `sim/tools/review_pages.py` with an
isolated port and report. It prints state/snapshot/failure counts and the
snapshot directory, not the page matrix. Read only failing or changed
snapshots; page and state selection follows the `ui-review` skill.

## Environment Checks

The wrapper reports `[ENV]` instead of dumping a CMake traceback for common
problems:

- missing `IDF_PATH` (`connectivity`, `sim`);
- missing `build/config/sdkconfig.h` for the simulator;
- uninitialized `contracts/device_link`;
- missing Pillow for page review;
- AStyle other than 3.6.19.

`doctor` checks those items plus the simulator libraries, monitor/coredump
tools, serial ports, and writable build/log directories. `--json` is the
machine-readable form; a required failure returns 1, warnings alone return 0.

`inspect` reads the latest log (or `--log PATH`) and prints the first build
error category, source location, and context. `--json` returns
`{category,first_error,diagnostics,log}`.

## Device Commands

`device status` reads the local build identity and coredump partition without
touching the board. `device logs --snapshot` captures a bounded monitor session
and fails when no serial output was captured; `--follow` streams until
interrupted. `device monitor --expect REGEX` uses the monitor's non-interactive
stdin command stream and returns `124` on timeout. `device crash` decodes the
flash coredump; build the firmware with `build idf --coredump` first so
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` is enabled. `device flash` requires
`--yes` and is never run automatically; `--monitor` verifies the boot identity
after writing and reports a verification failure as `verify (image written)`.
`device smoke` resets the board and waits for the boot banner. The banner's
`elf=` field is the configured short prefix of the ELF SHA-256; compare it as a
prefix of the full `ELF file SHA256` from `device status`. See
`device-operations` for the triage order.

## Adding a Suite

Add a standard standalone CMake project with `enable_testing()` and
`add_test()`. The wrapper discovers it on the next invocation. If it supports
sanitizers, expose exactly one `*_SANITIZER` cache variable with the values
`none`, `address`, and `thread`. Run `python3 tests/run_python_tests.py agent`
to validate discovery.

## Boundaries

The wrapper does not replace hardware validation and does not cache test
results. Re-run a gate when its inputs
change; do not treat a summary as evidence that an unrelated artifact is
current.

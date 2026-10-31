# Agent Workflow

`tools/agent/mt.py` is the command-line entry point for agent-facing build,
test, formatting, Git, simulator, and page-review output. It is a presentation
layer: the wrapped command decides pass or fail, and the wrapper never adds a
build or test gate.

## Why

A full simulator or IDF build prints thousands of progress lines, and CTest
prints two lines per passing test. Those lines remain in the session history and
are paid again on every later request. The wrapper keeps the complete output on
disk and prints a bounded summary with a log path.

## Commands

```sh
python3 tools/agent/mt.py list
python3 tools/agent/mt.py build apps
python3 tools/agent/mt.py test apps
python3 tools/agent/mt.py test integration
python3 tools/agent/mt.py test weather-service --profile address
python3 tools/agent/mt.py py configuration resources docs
python3 tools/agent/mt.py fmt --check
python3 tools/agent/mt.py git status
python3 tools/agent/mt.py git diff
python3 tools/agent/mt.py git diff --patch -- <paths>
python3 tools/agent/mt.py sim --scenario agent_contract
python3 tools/agent/mt.py ui --pages settings/wifi
```

`build` accepts a suite alias, `idf`, or `sim`. `test` builds first, then runs
CTest. `--profile none|address|thread` selects the sanitizer profile supported
by that suite; each profile uses a separate build directory.

## Suites

Host suites are discovered from their `CMakeLists.txt`:

- `main`, `bsp`, `apps`, `app-core`, `integration`, `connectivity`;
- one alias per `layers/middleware/components/*/tests/host`, using the component
  directory name with `_` replaced by `-` (for example `weather-service`).

A suite's sanitizer option is read from its `CMakeLists.txt`; do not guess the
name. `mt.py list` shows the alias and option. `connectivity` needs `IDF_PATH`.
Hardware-only work (`tests/display`) is intentionally not a wrapper suite.

Build directories live under `build/agent/host/<alias>/<profile>`. `build/sim`
and the root `build/` directory are reused, so incremental artifacts survive
across agent turns. `MT_BUILD_ROOT` overrides the host build root.

## Output Contract

Successful commands print one `[PASS]` line, optional warning counts, and the
log path. Failures print `[FAIL]` or `[TIMEOUT]`, the extracted diagnostics, and
the log path. Summaries are capped at 40 lines / 4 KiB. A compiler line is
capped at 2 KiB; the complete line stays in the log.

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
pull requests. `stats.jsonl` records only byte counts, line counts, duration, and
the exit code.

```sh
python3 tools/agent/mt.py log --last
python3 tools/agent/mt.py log --last --grep 'error:' --tail 20
python3 tools/agent/mt.py stats
```

When a failure is not clear from the summary, read the log with `mt.py log` or
`rg -n -C2 'error:|FAILED' <log>`; do not rerun the gate just to see output.

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

`ui` builds the simulator, then runs `sim/tools/review_pages.py --check` with an
isolated port and report. It prints state/failure counts and the screenshot
directory, not the page matrix. Read only failing or changed screenshots; page
and state selection follows the `ui-review` skill.

## Environment Checks

The wrapper reports `[ENV]` instead of dumping a CMake traceback for common
problems:

- missing `IDF_PATH` (`connectivity`, `sim`);
- missing `build/config/sdkconfig.h` for the simulator;
- uninitialized `contracts/device_link`;
- missing Pillow for page review;
- AStyle other than 3.6.19.

## Adding a Suite

Add a standard standalone CMake project with `enable_testing()` and
`add_test()`. The wrapper discovers it on the next invocation. If it supports
sanitizers, expose exactly one `*_SANITIZER` cache variable with the values
`none`, `address`, and `thread`. Run `python3 tests/run_python_tests.py agent`
to validate discovery.

## Boundaries

The wrapper does not replace hardware validation, does not run display
benchmarks, and does not cache test results. Re-run a gate when its inputs
change; do not treat a summary as evidence that an unrelated artifact is
current.

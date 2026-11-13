# Build and Verification

This document owns the build constraints, the simulator build gate, host-suite
selection, and the minimal verification tiers. [`doc/agent-workflow.md`](agent-workflow.md)
documents `mt.py` command behavior and logs; each suite's `CMakeLists.txt` is
authoritative for its build options.

## Build

```sh
git submodule update --init --recursive
idf.py set-target esp32s3 && idf.py build   # ESP32-S3 board, default product
idf.py set-target esp32p4  && idf.py build   # ESP32-P4 board, default product
idf.py -DMT_PRODUCT=<product-id> build       # explicit product after setting target
idf.py size   # only when the change touches cache, DMA reservation, or resources
```

One target is active at a time; `set-target` replaces the local `sdkconfig`.
Target-specific configuration lands in `sdkconfig.defaults.esp32s3` /
`sdkconfig.defaults.esp32p4`, shared values in `sdkconfig.defaults`
(`idf.py save-defconfig`); never edit `sdkconfig` by hand. The local `sdkconfig`
may hold private deployment values: back it up before switching targets.
`REQUIRES`/`PRIV_REQUIRES` must not depend on `CONFIG_xxx`; after a change to
source-file discovery run `idf.py reconfigure` instead of editing the generated
Ninja/CMake state.

## Simulator Build Gate

Every change must leave the host simulator buildable. The mandatory gate is:

```sh
python3 tools/agent/mt.py build sim
```

It configures and builds the canonical products under `build/sim-gate/`:
`s3_reference` with fake and real music, `p4_reference` with fake music, and
`minimal` with fake music. Panel geometry is derived from each product's board
manifest. A failure in any configuration fails the gate. Simulator usage and
internals are documented in [`sim/README.md`](../sim/README.md).

## Host Test Suites

Each host suite is a standalone CMake project; C sources use C11 and are compiled
with `-Wall -Wextra -Werror -Wpedantic` (the BSP suite also builds C++17):

```sh
python3 tools/agent/mt.py test <suite> [--profile none|address|thread]
```

Raw command for humans and CI:

```sh
cmake -S <suite path> -B /tmp/mt-<name> -G Ninja && cmake --build /tmp/mt-<name> && ctest --test-dir /tmp/mt-<name> --output-on-failure
```

`python3 tools/agent/mt.py list` shows the suite aliases and each suite's
sanitizer option. Suite discovery, build directories, and the output contract
are documented in [`doc/agent-workflow.md`](agent-workflow.md).

**Impact surface**: a suite is affected when this task changed any source file that the suite compiles. For example, changing a clock page source affects only `tests/integration`; `layers/apps/tests/host` does not compile it.

| Change location | Affected suites |
| --- | --- |
| `main/` | `main/tests/host` |
| `layers/bsp/` | `layers/bsp/tests/host` (includes C++); add `main/tests/host` when `main` links it |
| `layers/middleware/` | the affected component's `layers/middleware/components/*/tests/host`; add `main/tests/host` when `main` links it |
| `layers/apps/` (pages, app_ui) | `layers/apps/tests/host`, `tests/integration` |
| `layers/app_manager/` (app_core, app_theme) | `layers/app_manager/app_core/tests/host`, `tests/integration` |
| network runtime/connectivity | `tests/connectivity` |
| Kconfig/contract boundaries, resource manifests/assets | `python3 tools/agent/mt.py py configuration resources`; add `contracts` when the contract submodule changes |
| `doc/`, `AGENTS.md`, `.agents/skills/`, Markdown | `python3 tools/agent/mt.py py docs`; add `skill-tools` or `design-skills` when a skill helper changes |

`tests/integration` uses the none profile by default; sanitizers (the `*_SANITIZER` variable shown by `mt.py list`, set to `address` or `thread`) are enabled only when the change involves memory ownership or concurrency, and normally one profile matched to the risk. Host tests do not cover driver timing, RF, DMA, or power consumption; go to hardware when needed and record the verified scope.

## Verification Tiers

**Tiers** (matching "minimal verification"):

- L0 per edit batch: run `python3 tools/agent/mt.py fmt` over the changed files (idempotent) plus `python3 tools/agent/mt.py build <suite>` for each affected host suite.
- L1 per task: run `python3 tools/agent/mt.py test <suite>` once for each affected host suite.
- L2 before a UI change is done: `python3 tools/agent/mt.py sim` ×1 and `python3 tools/agent/mt.py ui --pages <affected>` ×1; the final review scope follows the scope table in `ui-review` and becomes full only when the change touches shared pieces (`app_ui`/`app_theme`/viewport copy), adds a page, or re-lays out a page.
- L3 commit/acceptance: `python3 tools/agent/mt.py fmt --check`, `python3 tools/agent/mt.py git diff --check`, `python3 tools/agent/mt.py build sim`, and the affected host suites; `--full` when reviewing formatting or sanitizer evidence.

Do not re-run a gate when the artifact under test did not change; validating a flaky fix means one serial run plus one parallel run. When a gate fails, read the summary first, then `python3 tools/agent/mt.py log --last --grep 'error:|FAILED'`; do not rerun only to see output.

## Device Faults

Use only the logs the user currently provides, and check in order: reset reason, panic backtrace, heap headroom, task WDT, boot placement, app state transitions. Built-in UI host regression uses `sim/ci/run_ci.sh`.

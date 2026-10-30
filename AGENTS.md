# Repository Guide

## Map

- `main/`: entry point, runtime, storage and network assembly. Resources are aggregated from the `layers/apps/*/assets/` and `layers/app_manager/app_theme/assets/` manifests and packaged into the `res` partition.
- `layers/`: `bsp`, `middleware`, `app_manager`, `apps` — four independent Git submodules. Changes inside a layer are committed and pushed in the submodule; the parent repository updates the pointer in the same commit.
- `sim/`: standalone host LVGL simulator, see [`sim/README.md`](sim/README.md). Agents bind `127.0.0.1:5002` only (5001 is reserved for the on-device baseline).
- `tests/`: `connectivity` and `integration` are cross-layer host tests; `display` is the on-device benchmark tool. `managed_components/` is read-only.

## Default Working Mode

- Change only code directly related to the task; analysis tasks are read-only; do not change code outside the plan, but report the problems you find. Conclusions come from code, tests, or documentation — never from guesses.
- **Minimal verification**: follow L0–L3 in "Host Tests and Verification Tiers" and run each gate once per task. Do not run the full-repository CTest, sanitizers, `idf.py build`/`size`, display benchmarks, or hardware checks by default; widen the scope only when the user explicitly asks for a commit, acceptance, or release verification, and state the widened part.
- **Do not add process gates**: never add CMake `FATAL_ERROR`, mandatory Kconfig switches, CI gates, or new "do not" rules to solve a problem. Keep the LVGL profile gate in the root `CMakeLists.txt` and the `sdkconfig.defaults` baseline as they are — neither weakened nor strengthened.
- Flash or erase only when the user explicitly asks.

## Build

```sh
git submodule update --init --recursive
idf.py set-target esp32s3 && idf.py build
idf.py size   # only when the change touches cache, DMA reservation, or resources
```

Configuration changes land in `sdkconfig.defaults` (`idf.py save-defconfig`); never edit `sdkconfig` by hand. `REQUIRES`/`PRIV_REQUIRES` must not depend on `CONFIG_xxx`; after a change to source-file discovery run `idf.py reconfigure` instead of editing the generated Ninja/CMake state.

## Host Tests and Verification Tiers

Each suite is a standalone CMake project (C11, `-Wall -Wextra -Werror -Wpedantic`) with the same flow:

```sh
cmake -S <suite path> -B /tmp/mt-<name> -G Ninja && cmake --build /tmp/mt-<name> && ctest --test-dir /tmp/mt-<name> --output-on-failure
```

**Impact surface**: a suite is affected when this task changed any source file that the suite compiles. For example, changing a clock page source affects only `tests/integration`; `layers/apps/tests/host` does not compile it.

| Change location | Affected suites |
| --- | --- |
| `main/` | `main/tests/host` |
| `layers/bsp/`, `layers/middleware/` | the matching `layers/*/tests/host` (bsp includes C++); add `main/tests/host` when `main` links it |
| `layers/apps/` (pages, app_ui) | `layers/apps/tests/host`, `tests/integration` |
| `layers/app_manager/` (app_core, app_theme) | `layers/app_manager/app_core/tests/host`, `tests/integration` |
| network runtime/connectivity | `tests/connectivity` |
| Kconfig/contract boundaries, resource manifests/assets | `python3 tests/run_python_tests.py configuration resources`; add `contracts` when the contract submodule changes |

`tests/integration` uses the none profile by default; sanitizers (`-D<suite>_SANITIZER=address|thread`) are enabled only when the change involves memory ownership or concurrency, and normally one profile matched to the risk. Host tests do not cover driver timing, RF, DMA, or power consumption; go to hardware when needed and record the verified scope.

**Tiers** (matching "minimal verification"):

- L0 per edit batch: run the `doc/code-style.md` §8.1 astyle pass over the changed files (idempotent) plus an incremental build of the affected projects.
- L1 per task: run each affected host suite once.
- L2 before a UI change is done: `sim/ci/run_ci.sh` ×1 and `sim/tools/review_pages.py --check` ×1; the final review scope follows the tiers in `ui-review` and becomes full only when the change touches shared pieces (`app_ui`/`app_theme`/viewport copy), adds a page, or re-lays out a page.
- L3 commit/acceptance: the full checklist in `doc/code-style.md` §8.2.

Do not re-run a gate when the artifact under test did not change; validating a flaky fix means one serial run plus one parallel run.

## Device Faults

Use only the logs the user currently provides, and check in order: reset reason, panic backtrace, heap headroom, task WDT, boot placement, app state transitions. Display benchmark thresholds and configuration are in `tests/display/README.md`; built-in UI host regression uses `sim/ci/run_ci.sh`.

## Coding Style

Follow [`doc/code-style.md`](doc/code-style.md): 4 spaces, Allman braces, UPPER_SNAKE macros, lower_snake functions, `_` prefix for file-static functions, Doxygen on header APIs, formatted with AStyle 3.6.19. Performance first; avoid needless defensive code, and do not add locks, checks, or a single return point for the sake of uniformity — a failure with no cleanup obligation returns directly, and `goto` to a single exit serves only functions that really must release, unlock, or roll back.

## Documentation Style

Reference documentation states implementation facts; it does not describe the implementation process or unimplemented content. A document covers only the module it owns and must not explain unrelated modules.

Language: agent-facing documents (`AGENTS.md`, `.agents/skills/`, `doc/`) and tool documents under `tests/` are English only. Every module `README.md` ships with a Simplified Chinese `README_ZH.md` twin, and both carry a language switcher on line 1: `**English** | [简体中文](README_ZH.md)` in the English file and `[English](README.md) | **简体中文**` in the Chinese one. A `README.md` edit and its `README_ZH.md` counterpart change in the same commit. Quoted product strings that appear on screen stay in their shipped language.

## Commits

Header `<type>(<scope>): <subject>`, body states motivation and impact, trailer `Refs:` or `BREAKING CHANGE:`; a PR lists changes, tests, and resource impact. A submodule pointer update must land in the same commit as the content change; pointer-only bumps are forbidden. Run the full checklist (`git diff --check` + astyle + affected host suites, see code-style.md §8.2) only when the user asks for a commit or acceptance.

## Skills

Load `.agents/skills/` on demand; do not preload everything:

| Scenario | Skill |
| --- | --- |
| App/Page lifecycle, routing, navigation, arguments | `app-development` |
| LVGL layout, text wrapping, clipping, touch/scroll propagation | `lvgl-ui-layout` |
| Adversarial simulator review before a UI change is done | `ui-review` |
| Board pins, buses, hardware constraints | `esp32` |
| Display pipeline ownership, draw submission, flush handshake | `lvgl-integration` |
| Firmware code size, map/archive analysis, build size comparison | `codesize` |

The general build, configuration, debugging, and verification workflow is defined in this file; do not load a skill for it.

Priority: this file and `doc/code-style.md` > current code, lock files, and topic documents > skill summaries; never override the current implementation with a constant from a skill.

# Repository Guide

## Map

- `main/`: firmware entry, runtime, storage/network assembly. Resources declared by `layers/apps/resource_manifest.cmake` (which aggregates each app's `resource_manifest.cmake`) and `layers/app_manager/app_theme/resource_manifest.cmake` are staged from their `assets/` directories and packaged into the `res` partition.
- `layers/`: `bsp`, `middleware`, `app_manager`, `apps` — four independent Git submodules. `bsp` owns board backends, `middleware` owns services, `app_manager` owns app lifecycle/theme/display power, and `apps` owns the built-in LVGL applications; each layer's `README.md` documents its components. Changes inside a layer are committed and pushed in the submodule; the parent repository updates the pointer in the same commit.
- `products/`: board/product/design-target composition manifests, see [`products/README.md`](products/README.md). Product selection uses `MT_PRODUCT` (empty selects the board's default product); board selection uses `MT_BOARD` (empty selects the target's only registered board).
- `sim/`: standalone host LVGL simulator, see [`sim/README.md`](sim/README.md). Agents bind `127.0.0.1:5002` only.
- `doc/`: build/verification, agent workflow, code style, configuration governance, design, colour, and audio-session guides; select by task below.
- `tests/`: cross-layer host tests under `connectivity/` and `integration/`, plus repository guard tests under `tests/docs/`, `tests/configuration/`, `tests/resources/`, and `tests/sim-build/`. `managed_components/` is read-only.
- `tools/agent/`: agent-facing command wrapper, see [`doc/agent-workflow.md`](doc/agent-workflow.md).

## Start Here

Load only the document that matches the task; do not preload unrelated topic documents.

| Task | Read |
| --- | --- |
| Set up, flash, or select a board/product | [`README.md`](README.md), [`products/README.md`](products/README.md) |
| Build, test, verify, or triage a device fault | [`doc/verification.md`](doc/verification.md) |
| Run agent commands (`mt.py`) or inspect logs | [`doc/agent-workflow.md`](doc/agent-workflow.md) |
| Write or review C/C++ | [`doc/code-style.md`](doc/code-style.md) |
| Change Kconfig, runtime policy, dependency pins, or board/product contracts | [`doc/configuration-governance.md`](doc/configuration-governance.md) |
| Work on a layer or component | the owning `layers/<layer>/README.md` and its component READMEs |
| Work on the simulator, Agent Bridge, or page review | [`sim/README.md`](sim/README.md) |
| Choose or change design profiles, targets, or visual language | [`doc/design/README.md`](doc/design/README.md), [`doc/color-language.md`](doc/color-language.md) |
| Change audio session or recording ownership | [`doc/audio-session-lease.md`](doc/audio-session-lease.md) |

## Default Working Mode

- Change only code directly related to the task; analysis tasks are read-only; do not change code outside the plan, but report the problems you find. Conclusions come from code, tests, or documentation — never from guesses.
- **Minimal verification**: use the tiers in [`doc/verification.md`](doc/verification.md) and run each gate once per task. Do not run the full-repository CTest, sanitizers, `idf.py build`/`size`, or hardware checks by default; widen the scope only when the user explicitly asks for a commit, acceptance, or release verification, and state the widened part.
- **Do not add process gates**: never add CMake `FATAL_ERROR`, mandatory Kconfig switches, CI gates, or new "do not" rules to solve a problem. Keep the LVGL profile gate in the root `CMakeLists.txt` and the `sdkconfig.defaults` baseline as they are — neither weakened nor strengthened. The simulator build gate in [`doc/verification.md`](doc/verification.md) is the sole exception because it enforces the required host-build contract.
- Flash or erase only when the user explicitly asks.

## Coding Style

Follow [`doc/code-style.md`](doc/code-style.md) for all C/C++ changes; format with `python3 tools/agent/mt.py fmt`.

## Documentation Style

Reference documentation states implementation facts; it does not describe the implementation process or unimplemented content. A document covers only the module it owns and must not explain unrelated modules.

Language: agent-facing documents (`AGENTS.md`, `.agents/skills/`, `doc/`) and tool documents under `tests/` are English only. Every module `README.md` ships with a Simplified Chinese `README_ZH.md` twin, and both carry a language switcher on line 1: `**English** | [简体中文](README_ZH.md)` in the English file and `[English](README.md) | **简体中文**` in the Chinese one. A `README.md` edit and its `README_ZH.md` counterpart change in the same commit. Quoted product strings that appear on screen stay in their shipped language.

## Commits

Header `<type>(<scope>): <subject>`, body states motivation and impact, trailer `Refs:` or `BREAKING CHANGE:`; a PR lists changes, tests, and resource impact. A submodule pointer update must land in the same commit as the content change; pointer-only bumps are forbidden. Run the full checklist (`git diff --check` + astyle + affected host suites + `python3 tools/agent/mt.py build sim`, see code-style.md §8.2) only when the user asks for a commit or acceptance.

## Skills

Load `.agents/skills/` on demand; do not preload everything:

| Scenario | Skill |
| --- | --- |
| App/Page lifecycle, routing, navigation, arguments | `app-development` |
| LVGL layout, text wrapping, clipping, touch/scroll propagation | `lvgl-ui-layout` |
| Adversarial simulator review before a UI change is done | `ui-review` |
| Design profiles, target capabilities, style switching | `design-contract` |
| New page visual direction, composition, typography, colour roles | `visual-design` |
| Aesthetic critique and profile-relative design QA | `aesthetic-review` |
| Press feedback, transitions, gestures and embedded motion budgets | `motion-interaction` |
| Task clarity, labels, errors, navigation and usability heuristics | `ux-heuristics` |
| Board pins, buses, hardware constraints | `esp32` |
| Display pipeline ownership, draw submission, flush handshake | `lvgl-integration` |
| Firmware code size, map/archive analysis, build size comparison | `codesize` |

The general build, debugging, and verification workflow is defined in [`doc/verification.md`](doc/verification.md); configuration contracts are in [`doc/configuration-governance.md`](doc/configuration-governance.md). Do not load a skill for either.

Priority: this file and `doc/code-style.md` > current code, lock files, and topic documents > skill summaries; never override the current implementation with a constant from a skill.

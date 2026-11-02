# Design Contract

This directory holds the style profiles, target capability profiles and
evaluation fixtures used by the project design skills. It is not a runtime
theme engine: it gives agents a stable way to describe a visual language and
the hardware/use target it must fit before they design or review a page.

## Three Layers

| Layer | What it owns | Where it lives |
| --- | --- | --- |
| Universal floor | Contrast, readable text, touch targets, state clarity, error recovery | `design-contract` skill rules |
| Style profile | Surface model, depth model, density, corner scale, type scale, decoration, motion level | `profiles/*.json` |
| Target profile | Display geometry, DPI, colour mode, refresh rate, input, CPU/RAM class, motion and asset budgets | `targets/*.json` |

A page is always evaluated against a resolved `(style profile, target profile)`
pair. Neither a panel size nor the current project palette is a universal
design truth.

## Selection Order

`design-contract` has two selection modes:

**Design mode** (`select --mode design`, default):

1. explicit `--profile` / `--target`;
2. `DESIGN_PROFILE` / `DESIGN_TARGET`;
3. `neutral` profile and `generic_constrained` target.

Design mode never reads `active.json`. The fallback is intentionally neutral
and constrained; a new page must not inherit the current UI by accident.

**Current-review mode** (`select --mode current`):

1. explicit `--profile` / `--target`;
2. `DESIGN_PROFILE` / `DESIGN_TARGET`;
3. `active.json`;
4. `neutral` profile and `generic_constrained` target.

`active.json` identifies the current UI under review. It is not a design
baseline, and the current palette/dimensions must not be copied into a new
profile or page.

## Profiles

| Profile | Intent | Production use |
| --- | --- | --- |
| `neutral.json` | Style-neutral accessible baseline used as fallback | Contract only |
| `dark_sparse.json` | The dark, sparse, emissive-inspired reference style | Current-UI subject; not a baseline |
| `paper_light.json` | Light paper surfaces, border-led depth, warm accent | Validation fixture |
| `mono_industrial.json` | Monochrome, compact, square, high-contrast operations UI | Validation fixture |
| `soft_playful.json` | Soft, spacious, rounded, expressive consumer UI | Validation fixture |

Only `dark_sparse.json` has a compatibility mapping to the current
`app_theme_colors.h` and `app_ui_theme.h`. Other profiles are test fixtures,
not claims that the production UI can render them.

## Targets

| Target | Class | Production use |
| --- | --- | --- |
| `panel_368x448_touch_60hz.json` | Current panel, touch, RGB565 | Supported renderer |
| `panel_320x480_touch_30hz.json` | ESP32-P4 3.5-inch IPS, touch, RGB565 | Supported renderer |
| `generic_constrained.json` | Unknown constrained touch target | Fallback fixture |
| `panel_240x320_touch_30hz.json` | Small, low-refresh touch target | Validation fixture |
| `panel_480x480_touch_60hz.json` | Larger, standard-refresh touch target | Validation fixture |
| `panel_128x64_mono_keypad.json` | Monochrome, non-touch, keypad target | Validation fixture |

Synthetic targets exist to prove that skills do not depend on the current
panel parameters. They do not authorize hardware changes. The current panel
target's 6.0 mm touch floor is preserved as an explicit target contract; the
absolute universal floor is 4.5 mm. Current UI findings are
listed in `current-ui-findings.md` and are not used to lower the floor or derive
a profile.

## Using the Skills

| Task | Skill |
| --- | --- |
| Create, validate, select or switch a design profile | `design-contract` |
| Plan a new page or restyle within a profile | `visual-design` |
| Review visible UI against a profile | `aesthetic-review` |
| Plan or audit press feedback, transitions and gestures | `motion-interaction` |
| Review task clarity, labels, errors and navigation | `ux-heuristics` |

`visual-design` and `aesthetic-review` do not own LVGL layout mechanics. Use
`lvgl-ui-layout` for Fit/Grid, wrapping, clipping and scroll ownership.
`aesthetic-review` consumes the existing `ui-review` evidence; it does not
replace the simulator harness.

## Validation

```sh
python3 .agents/skills/design-contract/scripts/design_profile.py validate \
    --profiles-dir doc/design/profiles --targets-dir doc/design/targets
python3 .agents/skills/design-contract/scripts/design_profile.py check-current
python3 tests/run_python_tests.py design-skills docs skill-tools
```

`check-current` compares `dark_sparse.json` with the C headers that own the
current colours and metrics. It fails on drift.

Behavioural evaluation cases live in `evals/cases/`; their scoring rubric is
`evals/rubric.md`, and the 25-run complete style/target matrix is
`evals/matrix.json`. Each run declares `expect` as `ok`, `warning`, or an
expected error code, plus `expect_codes` for warning runs. Warnings are
capability downgrades, not harness failures. `evals/check_output.py` checks a
saved skill output against a case's `must`/`must_not` lists and rejects raw
colours, current-hardware literals, and bare panel dimensions; target ids are
masked before that dimension check.

## Current Implementation Limits

- `app_ui.c` still has a fixed page shell (64 px title bar, 420 px sheet cap,
  fixed row heights). Multi-target profiles can describe a future target, but
  the current implementation claims `panel_368x448_touch_60hz` and
  `panel_320x480_touch_30hz`.
- `app_theme` exposes seven semantic font roles and one font asset. A profile
  whose type scale cannot be mapped to those roles reports
  `TYPOGRAPHY_UNSUPPORTED` in the resolved context.
- `style_audit` checks palette roles (including visible border colours),
  contrast, radius, spacing, touch targets, distinct known image ids and opaque
  accent-colour count. It skips hidden/off-screen nodes; palette, contrast,
  radius and spacing checks still run on non-clickable nodes, while touch checks
  require `flags.clickable`. It reports unknown images separately, uses
  `styles.ext_click_pad` plus clickable-row bounds for composite controls, and
  audits `overlays` when a wrapper includes them. `--check` fails on any
  finding; `--no-spacing` skips computed padding when theme defaults or optical
  exceptions make token comparison unreliable.
  `max_image_bytes`, shadow and gradient provenance remain review budgets; the
  tree does not expose enough provenance to prove them automatically.
- Runtime theme switching is not implemented. The skills work at design-time
  and review-time; a later, separately approved change may add a compile-time
  profile preview.
- `supported: true` means the implementation can render the target; it does
  not mean the current pages are visually compliant. `style_audit` findings on
  current pages are review evidence, not exceptions to the universal floor.

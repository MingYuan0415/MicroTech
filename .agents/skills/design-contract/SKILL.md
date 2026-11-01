---
name: design-contract
description: Resolve, validate, compare and switch MicroTech design profiles and target profiles. Use when choosing a visual language, adding a style variant, changing target capabilities, or when another design skill needs a profile/target context before planning or reviewing UI.
---

# MicroTech Design Contract

The design contract separates three things that must not be conflated:

1. **Universal floor** — contrast, touch targets, readable text, state clarity,
   error recovery. These apply to every style and target.
2. **Style profile** — a named visual language: surface model, depth model,
   density, corner scale, type scale, decoration and motion level.
3. **Target profile** — display, input and resource capabilities: geometry,
   DPI, colour mode, refresh rate, CPU/RAM class, motion and asset budgets.

A page is always resolved against `(style profile, target profile)`.
`dark_sparse` is one profile in this repository, not a universal baseline.

## Sources of Truth

- `doc/design/README.md` explains the contract and current implementation limits.
- `doc/design/profiles/*.json` are style profiles.
- `doc/design/targets/*.json` are target capability profiles.
- `doc/design/active.json` selects the current project pair.
- `scripts/design_profile.py` is the only supported resolver and validator.
- `references/inspiration.md` records provenance and scope.

Read the owning profile and target before changing a page. Do not copy hex
values or pixel values from the current theme into a page or another skill.

## Workflow

1. **Resolve a context.** Run:

   ```bash
   python3 .agents/skills/design-contract/scripts/design_profile.py resolve \
       --profile <profile-id> --target <target-id> --output <context.json>
   ```

   If the caller did not choose a pair, run `select --mode design`. This mode
   uses the explicit argument first, then the environment variable, then
   `neutral` + `generic_constrained`; it never reads `active.json`. To audit
   the current UI, run `select --mode current`, which adds `active.json` after
   the explicit and environment choices. Its `selection` object records
   `mode` and which source won for each side. `active.json` is a review
   subject, not a design baseline.
2. **Read the resolved context.** It contains semantic colours, space/radius/type
   scales, minimum touch size, motion budget, effect permissions and any
   downgrade findings. Treat the resolved values as the design input.
3. **Author a new style when needed.** Follow `references/switching.md`, copy
   `neutral.json`, change the style axes and roles, keep the schema version,
   run `validate`, then run `diff` against an existing profile. A new style
   that only changes colours is not a new style; at least three style axes
   must differ.
4. **Validate before use.** `validate` checks schema, role completeness,
   contrast pairs, monotonic scales, motion budget and all profile × target
   pairs (`pairs` in its JSON output). Expected incompatibilities such as a
   colour profile on a mono target are reported with their error code.
5. **Record the context in the handoff.** Every design or review output must
   name `profile_id`, `profile_version`, `target_id` and any downgrade findings.

## Commands

```bash
python3 .agents/skills/design-contract/scripts/design_profile.py validate
python3 .agents/skills/design-contract/scripts/design_profile.py resolve \
    --profile paper_light --target panel_240x320_touch_30hz --output .cache/design-context.json
python3 .agents/skills/design-contract/scripts/design_profile.py select --mode design
python3 .agents/skills/design-contract/scripts/design_profile.py select --mode current
python3 .agents/skills/design-contract/scripts/design_profile.py diff paper_light soft_playful
python3 .agents/skills/design-contract/scripts/design_profile.py check-current
```

`check-current` compares `dark_sparse` with the C headers that own the current
colours and metrics. A mismatch is drift, not a style choice.

## Switching Rules

- Explicit `--profile` / `--target` wins over everything.
- Environment variables are the second choice for CI or temporary sessions.
- `select --mode design` never reads `active.json`; with no explicit or
  environment pair it falls back to `neutral` + `generic_constrained`.
- `active.json` is the current UI review subject, not a design baseline.
  Only `select --mode current` may select it.
- In `current` mode, `active.json` version pins must match the loaded
  profile/target; a mismatch is `VERSION_MISMATCH`, not a silent upgrade.
- Unknown or missing profile/target must fail with `UNKNOWN_PROFILE` or
  `UNKNOWN_TARGET`; do not guess and do not reuse the previous context.
- A style profile may declare `requires.color_modes` and
  `requires.min_refresh_hz`. If the target cannot satisfy them, stop and report
  `COLOR_MODE_UNSUPPORTED` or `REFRESH_TOO_LOW`.
- Motion is resolved as `min(style.motion_level, target.max_motion_level)`.
  Record `MOTION_DOWNGRADED` when the target caps the style.
- Shadow, blur and gradient permissions are the intersection of style rules
  and target capabilities. Record `EFFECT_DOWNGRADED` when the target removes
  an effect.

## Prohibited

- Do not create a profile with raw values copied from a page.
- Do not add a new style axis outside `references/profile-schema.md` without
  updating the schema, validator, tests and all profiles.
- Do not claim that a synthetic target is supported hardware. `supported` must
  remain false for fixtures.
- Do not use the current panel geometry or current palette as an implicit
  fallback.
- Do not add CMake/Kconfig gates for design profiles; this is a skill and test
  contract, not a build gate.

## Handoff

Report the resolved pair, the scale values in semantic terms, any capability
downgrade, and the remaining implementation limits (for example, a type scale
that cannot be mapped to the current font roles).

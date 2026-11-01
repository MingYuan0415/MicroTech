# Profile Switching

Switching is explicit. A design skill must never infer a style from the current
source tree, the panel technology or the previous task.

## Resolution Order

### Design mode

1. `--profile` / `--target`
2. `DESIGN_PROFILE` / `DESIGN_TARGET`
3. `neutral` + `generic_constrained`

Design mode never reads `active.json`. The fallback is deliberately neutral and
constrained; a new page cannot inherit the current production UI by accident.

### Current-review mode

1. `--profile` / `--target`
2. `DESIGN_PROFILE` / `DESIGN_TARGET`
3. `doc/design/active.json`
4. `neutral` + `generic_constrained`

Current-review mode is for auditing the existing UI. The active pair is the
subject under review, not a design standard.

## Switching a Page

1. Choose a profile and target.
2. Run `design_profile.py select --mode design` for a new design, or
   `select --mode current` for the current UI, and keep the `selection` mode
   and source.
3. Read the resolved context before touching UI code or writing a review.
4. Keep the pair for the whole task. Do not switch profiles halfway through a
   page review; if the request changes, resolve again and state the change.
5. Record the pair, the selection mode/source and any `MOTION_DOWNGRADED` /
   `EFFECT_DOWNGRADED` findings in the handoff.

## Creating a Style Variant

1. Start from `neutral.json` (not `dark_sparse.json`).
2. Change at least three style axes; a palette-only edit is not a new style.
3. Keep existing role names; add a role only if the schema and validator are
   updated in the same change.
4. Run `validate` and `diff <existing> <new>`.
5. Mark `fixture: true` unless the profile is approved for production.

## Creating a Target

1. Describe capabilities, not a product name or a board name.
2. Set `supported: false` and `fixture: true` unless the implementation has been
   verified on that target.
3. Set conservative resource values when unknown: `max_motion_level: none` or
   `micro`, no blur/shadow/gradients, small image and icon budgets.
4. Run `validate` and a `resolve` against each style profile that claims to
   support it.

## Style and Target Compatibility

- Colour mode: a style that needs `rgb565` cannot be resolved against a `mono`
  target. Report the incompatibility; do not approximate colour with grayscale
  silently.
- Refresh rate: a style that requires a minimum refresh rate fails on a slower
  target. A lower-motion variant may be created instead.
- Touch: when the target has no touch input, the style must not rely on touch
  target sizes; the UX skill must use keypad/encoder rules.
- Motion: the resolved level is always the minimum of style ambition and target
  capability. Never raise a target's budget to satisfy a style.

## Anti-Patterns

- “Use the project style” without naming a profile.
- “It is the current panel width, so use the current style.”
- Switching to a new profile during review to make a finding disappear.
- Copying a profile's hex or px values into `SKILL.md` or page code.
- Treating a synthetic fixture as supported hardware.

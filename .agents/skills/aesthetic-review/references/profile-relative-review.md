# Profile-Relative Review

The same screen may be excellent in one profile and wrong in another. Use the
active profile's axes as the expectation, and the universal floor as the
non-negotiable part.

## Examples

### `dark_sparse`

- Tonal surfaces and lightness-led depth are expected.
- Sparse decoration; a bright accent is meaningful, not decorative.
- Low average luminance is intentional, but body text still meets the numeric
  contrast floor.
- On a target with `display.emissive: true`, large near-white chrome is a
  finding because it wastes luminance and risks differential ageing. Do not
  apply this hardware rule to non-emissive targets.

### `paper_light`

- Light surfaces and border-led depth are expected.
- Shadows are not required; if the profile disallows them, a shadow is a
  profile violation, not a quality upgrade.
- Warm paper tint is intentional; do not “correct” it to pure white.
- Contrast is measured, not assumed from the light background.

### `mono_industrial`

- Monochrome is expected. A missing accent is not a defect.
- Square corners, compact density and border separation are intentional.
- Status must be carried by text, weight, pattern or icon, never hue alone.
- Touch assumptions do not apply when the target is keypad-only.

### `soft_playful`

- Rounded corners, spacious density and expressive decoration are intentional.
- Decoration is allowed only when it supports hierarchy; it is not a free pass
  for clutter.
- Motion may be more expressive, but the target cap still wins.
- Contrast and touch targets remain universal.

### `neutral`

- A style-neutral baseline, not a brand direction.
- Use it when no profile is supplied; report that fallback explicitly.
- Do not treat it as the project's permanent visual identity.

## Mismatch Handling

If the screenshot/tree shows a different surface model, palette or radius
language than the selected profile:

1. Report `PROFILE_MISMATCH` as a blocker.
2. State which profile the evidence appears to match, if obvious.
3. Do not continue with style-specific findings until the correct pair is
   resolved.

A profile mismatch can be a legitimate test case, but it is never a reason to
judge the page against the wrong style.

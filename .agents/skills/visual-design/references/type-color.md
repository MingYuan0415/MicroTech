# Type and Colour Roles

## Use Roles, Not Values

`design-contract` resolves a profile into semantic roles and relative scales.
The design plan names roles, never literals:

- surfaces: `canvas`, `surface_1`, `surface_2`, `surface_pressed`, `surface_low`;
- lines: `border`, `track`, `focus`;
- text: `ink`, `ink_muted`, `ink_faint`, `on_accent`;
- action/status: `accent`, `success`, `warning`, `danger`, `accent_tint`,
  `success_tint`, `warning_tint`, `danger_tint`.

The implementation binds those roles to the target's concrete values. A page
must not contain raw hex or a colour literal.

## Type Scale

Choose a small hierarchy and keep it closed:

- one body size;
- one caption/support size;
- one title size;
- one display size, only if the page has a genuine hero fact;
- one larger numeric size for clocks, results or counters when justified.

Do not create a new size for each page. Do not use display type for labels,
buttons or data tables. A profile's `type_scale` axis decides whether hierarchy
is compact, standard or editorial.

For mixed Chinese and Latin text, verify the real font chain and line height;
nominal point size is not a measurement. `lvgl-ui-layout` owns the text policy.

## Colour

- One accent per surface, used for primary action, current selection and state.
- Status colours carry semantics; never use them as decoration. The app-layer
  alias `APP_UI_COLOR_WARNING` is a historical danger alias; do not use it for
  the profile's semantic `warning` role.
- Colour is never the only signal. Pair it with text, icon shape or pattern.
- Use contrast for hierarchy before adding more hues.
- Use the smallest number of surface steps the composition needs.
- A dark profile does not mean pure black; a light profile does not mean pure
  white. The profile's roles decide.
- A monochrome target uses text, weight and pattern instead of hue.

## Contrast

The resolved context carries `min_contrast_body`, `min_contrast_large`,
`min_contrast_ui` and `min_contrast_faint`. Check the combinations that actually
touch, not only the palette table. If a state colour fails on a surface, change
the role or the surface, not the text size.

## Iconography

- One icon family, one stroke language, one optical size.
- Icons supplement labels; an icon-only control needs an accessible name or a
  visible label nearby.
- Do not use emoji as structural icons.
- Do not mix filled and outline icons at the same hierarchy level.
- Keep icons inside the target's `max_icon_count` and `max_image_bytes` budget.

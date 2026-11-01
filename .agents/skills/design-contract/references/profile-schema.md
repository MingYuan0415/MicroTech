# Design Profile Schema

Profiles are JSON documents under `doc/design/profiles/`. Targets are JSON
documents under `doc/design/targets/`. The validator in
`scripts/design_profile.py` is the executable schema.

## Style Profile

| Field | Meaning |
| --- | --- |
| `schema_version` | Must be `1`. |
| `id` | Stable lowercase identifier; never reuse an id for a different style. |
| `version` | Positive integer; bump on any role/axis change. |
| `display_name` | Human-readable name. |
| `style_axes` | The ten visual axes listed below. |
| `roles` | Semantic colour roles; every value is `#RRGGBB`. |
| `scales` | Relative space, radius and type scales. |
| `rules` | Contrast floors (body, large, UI, faint), touch minimum, effect permissions, contrast pairs. Minimums are absolute and cannot be lowered below the universal floor. The validator enforces body 4.5, large/UI 3.0, faint 4.0 and touch 4.5 mm as absolute minima. Each contrast pair declares its floor. |
| `requires` | Target capabilities the style needs. |
| `compatibility` | Optional mapping to the current C headers; only for the current production style. May include `colors`, `app_aliases` and `metric_checks`. |
| `fallback` | True only for `neutral`. |
| `fixture` | Required boolean; true when the profile is a validation fixture, not a production theme. |

### Style Axes

| Axis | Values | Meaning |
| --- | --- | --- |
| `surface_model` | `flat`, `tonal`, `elevated`, `outlined` | How surfaces separate from the canvas. |
| `depth_model` | `none`, `border`, `lightness`, `shadow` | How elevation is expressed. |
| `density` | `compact`, `comfortable`, `spacious` | Information density and rhythm. |
| `corner_scale` | `square`, `soft`, `rounded`, `pill` | Corner language. |
| `type_scale` | `compact`, `standard`, `editorial` | Type hierarchy and contrast. |
| `decoration` | `none`, `sparse`, `expressive` | Amount of non-informational decoration. |
| `motion_level` | `none`, `micro`, `standard`, `rich` | Desired motion ambition before target capping. |
| `contrast_profile` | `normal`, `high`, `maximum` | Contrast intent; rules still enforce the numeric floor. |
| `icon_style` | `outline`, `filled`, `geometric` | Icon family character. |
| `color_temperature` | `cool`, `neutral`, `warm` | Palette temperature, not a fixed palette. |

### Roles

Required roles: `canvas`, `surface_1`, `surface_2`, `surface_pressed`,
`surface_low`, `border`, `track`, `ink`, `ink_muted`, `ink_faint`, `accent`,
`on_accent`, `success`, `warning`, `danger`, `focus`, `accent_tint`,
`success_tint`, `warning_tint`, `danger_tint`.

Pages must use roles, not raw values. A page-level exception is allowed only
when the skill records the role override and the reason; even then the value
must come from the profile, not a literal.

### Scales

`scales.space`, `scales.radius` and `scales.type` are **multipliers**, not
pixels. The target profile supplies `base_space_px`, `base_radius_px` and
`base_font_px`; `resolve` produces the concrete values. This is what keeps a
profile independent of one panel.

`scales.type` requires `caption`, `body`, `title`, `heading`, `display` and
`hero`, strictly increasing. The implementation maps these roles to its
available font roles or reports `TYPOGRAPHY_UNSUPPORTED`.

`contrast_pairs` entries are objects:

```json
{"foreground": "ink_faint", "background": "surface_1", "floor": "faint"}
```

The `floor` is one of `body`, `large`, `ui` or `faint`; the validator applies
the corresponding `rules.min_contrast_*` value. A pair's floor may not be lower
than the universal minimum for that role.

### Target Profile

| Group | Fields |
| --- | --- |
| `display` | `width`, `height`, `dpi`, `color_mode`, `refresh_hz`, `orientation`, optional `emissive` |
| `input` | `touch`, `keypad`, `encoder`, `min_touch_mm` |
| `resources` | `cpu_class`, `ram_class`, `base_space_px`, `base_radius_px`, `base_font_px`, `available_font_px`, `typography_tolerance_px`, `max_motion_level`, `max_motion_ms`, `max_animated_objects`, `allow_blur`, `allow_shadow`, `allow_gradients`, `max_image_bytes`, `max_icon_count` |
| `supported` | True only for a target the current implementation can render. |
| `fixture` | True for synthetic validation targets. |

`color_mode` is `rgb565`, `grayscale` or `mono`. A style profile declares which
modes it can survive in `requires.color_modes`; a monochrome target cannot
silently receive a colour-only style.

`max_icon_count` is a target resource. `max_accent_roles` is a style-profile
rule. `style_audit` checks the icon budget from distinct visible image semantic
ids and the accent budget from opaque accent-colour fills. `min_contrast_ui`
is a design rule for icon/border contrast; the tree does not expose enough
geometry/colour provenance to prove it automatically. `max_image_bytes`,
shadow and gradient permissions remain design budgets that require
source/asset review; they are not a complete automatic gate.

### Error Codes

| Code | Meaning |
| --- | --- |
| `INVALID_PROFILE_OR_TARGET` | Schema, roles, scale or contrast failure. |
| `UNKNOWN_PROFILE` / `UNKNOWN_TARGET` | The requested id is not in the directory. |
| `VERSION_MISMATCH` | `active.json` pins a profile/target version that does not match the loaded file. |
| `COLOR_MODE_UNSUPPORTED` | Style requires a colour mode the target does not provide. |
| `REFRESH_TOO_LOW` | Target refresh rate is below the style requirement. |
| `MOTION_DOWNGRADED` | Target capped the style motion level. |
| `EFFECT_DOWNGRADED` | Target removed shadow, blur or gradient permission. |
| `TYPOGRAPHY_UNSUPPORTED` | The type scale cannot map to available font roles. |
| `MISSING_COMPATIBILITY` / `MISSING_ROLE` / `MISSING_MACRO` | `check-current` cannot find the profile/header mapping. |
| `COLOR_DRIFT` / `METRIC_DRIFT` / `ALIAS_DRIFT` | `check-current` found a difference between profile, C header or app alias. |
| `DUPLICATE_PROFILE` / `DUPLICATE_TARGET` | Two files declare the same stable id. |
| `INVALID_SELECT_MODE` | `select` was asked for a mode other than `design` or `current`. |

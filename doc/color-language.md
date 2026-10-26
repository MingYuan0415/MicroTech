# Emissive Color Language (AMOLED)

The 368x448 SH8601A is an emissive panel: a `#000000` pixel is fully off. The
whole UI therefore organizes color around "light is hierarchy" — contrast, power,
and visual quality all come from sparse emission on a dark canvas rather than from
lifted gray blocks.

## Single Source

Raw color values may appear only in
`layers/app_manager/app_theme/include/app_theme_colors.h`
(`APP_THEME_COLOR_*`). Mapping inside the layers:

- `app_core` (task switcher, back gesture) includes that header directly;
  `app_core` REQUIRES `app_theme` and has no extra dependency.
- The app layer's `layers/apps/common/include/app_ui_theme.h` is a thin alias
  layer, `APP_UI_COLOR_*` → `APP_THEME_COLOR_*`; each App's own `*_COLOR_*`
  internal aliases point at it in turn. Page code must not contain color
  literals.
- Host regression: `layers/apps/tests/host/theme_contrast_test.c` asserts the
  minimum contrast ratio of every fg/bg pair against WCAG relative luminance and
  pins the language invariants such as a true-black canvas and a monotonic
  lightness ramp. Run it before adjusting the palette to predict a mismatch.

## Palette

| Role | Token (`APP_THEME_COLOR_*`) | Value | Notes |
| --- | --- | --- | --- |
| Canvas | `VOID` | `#000000` | Full-screen background; pixels off, mean luminance is power |
| Card | `PLUME` | `#14191E` | Sparse dark layer, only where grouping is needed |
| Pressed/selected base | `PLUME_HI` | `#1E262D` | Press **brightens**, never darkens |
| Icon recess | `SUNKEN` | `#0A0E11` | Icon-chip base below PLUME |
| Hairline | `HAIRLINE` | `#232C33` | 1 px dividers, non-semantic graphics, gesture-indicator base |
| Primary text | `INK` | `#EAF0F2` | 18.2:1 on VOID; deliberately below pure white to control bloom |
| Secondary text | `INK_SOFT` | `#A9B6BC` | 10.1:1 on VOID |
| Text on accent | `ON_ACCENT` | `#04141A` | Text and icons on any bright accent fill |
| Primary accent (cyan) | `AZURE` | `#43C6DB` | Actions, active state, charging; 10.3:1 |
| Time/standby (amber) | `AMBER` | `#FFB454` | Sun, waiting, time semantics; 11.9:1 |
| Success | `MINT` | `#4FDD8B` | Success, target met; 12.0:1 |
| Danger | `CORAL` | `#FF6B6B` | Destructive actions, faults; 7.6:1 |

`app_ui_theme.h` keeps historical names (`RAIN`=AZURE, `SUN`=AMBER,
`WARNING`=CORAL, and so on). The old `WARNING_BG` is removed: warning banners are
now an accent tint (see below).

## Usage Rules

1. **Two depth steps**: VOID → PLUME → PLUME_HI plus hairline outlines; never
   introduce a third gray background level.
2. **Selected = tint + outline**: use the accent as `bg_color` with a low
   `bg_opa` (≈20%), optionally with a 1 px accent border (`LV_OPA_40`). Do not use
   a solid gray block for selection (the weather alert banner uses exactly this
   form).
3. **Press brightens**: the press state takes the brighter step (`PLUME` →
   `PLUME_HI`); on an emissive panel the feedback is "lit up".
4. **On accent, use only `ON_ACCENT`**: switch knobs in the checked state,
   inverted button text, and similar use near-black to guarantee ≥7:1.
5. **Spend brightness on information**: no large, permanently bright blocks across
   the viewport; text uses INK instead of pure white; static chrome (dial hands,
   switcher bar) emits only through thin strokes to reduce differential aging
   (burn-in) risk.
6. **Never carry semantics by hue alone**: an accent state also has an icon or
   text; the four hues are fixed to action/time/success/danger with no second
   meaning.

## Verification

A palette-level change must pass `theme_contrast_test` (host) +
`sim/ci/run_ci.sh` + `sim/tools/review_pages.py` (regenerate the baseline together
with the palette).

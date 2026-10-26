# App Image Asset Workflow (SVG-first)

App images in the repository have SVG sources only; no PNGs are committed, and
none may be added. At build time `cmake/mt_app_resources.cmake` runs the whole
`SVG → tools/asset_pipeline/svg2png.py (PyMuPDF) → RGBA PNG → LVGLImage.py →
RGB565A8 BIN → res partition (mmap)` chain, so firmware and sim share one rule.

## Authoring rules for an SVG source

- **Art must fall inside the window the consumer actually draws.** A plain
  LVGL `lv_image` draws at native size and **center-crops** inside the object
  box without scaling: when the exported record size is larger than the
  drawing size, the edges are silently cut off. App icons are drawn in a
  64-unit design box and exported at 72: the launcher's
  `app_ui_add_icon_tile` gives a canvas of 72
  (`lv_obj_set_size(image, 72, 72)`) and the task switcher gives 56, so every
  element must fall inside `x,y = 13..50` centered in the design box; home
  info-card glyphs appear only in a 40 px window, so they are exported at 40;
  `home_ring_clock.svg` appears only inside a 28 px ring, so it is exported at
  28. After adding or changing an icon, run
  `tests/resources/test_icon_fit.py` (it records each drawing window by token
  and, after rasterization, verifies the ink box must not touch the window).
- Final colors are written into the file: `fill`/`stroke` must be explicit hex
  (taken from the palette in
  `layers/apps/common/include/app_ui_theme.h`); `currentColor` is rejected by
  `svg2png.py` and fails the build.
- A `viewBox` is mandatory; the export size is not written in the SVG but
  decided by the manifest's width/height fields (rasterization uses 4×
  oversampling, so any target size comes from the same source).
- Draw on a 16 or 24 grid (matching the QWeather icon style) on a transparent
  background, and use `stroke-linecap="round"` for geometric stroke endpoints.

## Registering the asset

1. Put the source file in the owning App's `assets/` directory (semantic name,
   for example `tile_calculator.svg`).
2. Register a 6-field record for it, one by one, in that App's
   `resource_manifest.cmake`:
   `"${CMAKE_CURRENT_LIST_DIR}/assets/<name>.svg|<staged output name>.png|APP_IMAGE_<X>|W|H|SVG"`;
   when one source is needed at several sizes, register several records with
   mutually distinct output names.
3. Allocate the new `APP_IMAGE_*` ID inside the owning App's reserved range in
   `layers/apps/common/include/app_image_ids.h` (home tiles use
   0x5770-0x577F).
4. UI code obtains the descriptor through `app_manager_get_image()` and keeps
   the LVGL symbol fallback.

## Vendoring QWeather icons

Weather condition icons are fixed to the `N-fill.svg` variants from
`~/esp/project/Icons` (qwd/Icons, MIT): copy `icons/<code>-fill.svg`, replace
`currentColor` with the theme color, rename it to `weather_<slot>.svg`, keep
`qweather-icons-LICENSE.txt`, and note the origin in the manifest header. The
condition code→slot mapping changes only in `app_weather_ui.c`, never in the
resources.

## Validation

- After `idf.py reconfigure` the resource build stage validates the entire
  rasterization chain; you must reconfigure after changing the manifest.
- Host contract: `python3 -m unittest discover -s tests/resources` (the SVG
  set, no leftover bitmaps, manifest records and sources exist, and **every
  icon's ink must fall inside all of its drawing windows**).
- Visually: a sim screenshot (`sim/ci/run_ci.sh` or a dev session) confirms
  that 40px small icons do not bleed color and that 112px large images show no
  jaggies; the build python needs Pillow + PyMuPDF
  (`requirements-weather-assets.txt`).

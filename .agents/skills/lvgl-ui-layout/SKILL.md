---
name: lvgl-ui-layout
description: Design, implement, review, and repair adaptive LVGL 9 user interfaces in MicroTech, with emphasis on Flex/Grid sizing, content-driven containers, text measurement and wrapping, clipping and overlap prevention, scroll ownership, touch propagation, and efficient label construction. Use when an LVGL page or widget clips text, overlaps controls, scrolls incorrectly, loses touch gestures over children, relies on absolute coordinates, uses truncating label modes, or needs responsive layout and host-test coverage.
---

# LVGL UI Layout

Build layout contracts from the outside in. Treat every dynamic label, flex
track, scrollable axis, and input target as an explicit constraint rather than
fixing visible symptoms with additional coordinates.

## Establish the Current Contract

1. Read the nearest `AGENTS.md`, the locked LVGL version, project theme/fonts,
   target display dimensions, parent page helper, and relevant host fakes.
2. Inspect the complete object tree and all style padding, border, gap, width,
   height, flex/grid, scroll, and event flags that affect it. Do not reason from
   a cropped source fragment.
3. Read [references/lvgl9-layout-patterns.md](references/lvgl9-layout-patterns.md)
   when budgeting a page against the viewport, selecting a label policy,
   or changing event and scroll propagation.
4. Optionally run `scripts/audit_lvgl_layout.py <paths>` to locate suspicious
   absolute positioning, truncation, label setup order, and clickable labels.
   Treat findings as review prompts, not proof of defects. Trace dynamic font,
   symbol, and object-helper arguments manually when the script reports an
   indirect role or cannot see the call site.

## Design Outside In

1. Define the page's usable content rectangle after header, safe area, border,
   padding, and scrollbar reservation.
2. Give each axis one owner. Use Flex for ordered rows/columns and Grid for
   aligned two-dimensional data. Use alignment for overlays and anchors.
3. Use percentages only against a parent with a resolved size. Do not put a
   percentage-sized child on the same axis as an `LV_SIZE_CONTENT` parent;
   that creates a circular size dependency.
4. Use `LV_SIZE_CONTENT` for content-driven containers and unconstrained short
   labels. Use `lv_obj_set_flex_grow()` or Grid fractional tracks for remaining
   space. Budget fixed children, gaps, padding, and borders before assigning
   flexible space.
5. Keep fixed pixels only where the invariant is real: touch targets, bitmap
   dimensions, chart/canvas geometry, bounded controls, or font-derived line
   boxes. Document the invariant and test its worst case. Do not use guessed
   coordinates to assemble ordinary page content.
6. Decide fit before scroll. Measure the page's vertical extent (last child
   bottom plus `pad_bottom`, which counts toward the scrollable range) against
   the viewport. When everything fits with a clean composition, disable
   scrolling explicitly (`LV_DIR_NONE` and clear `SCROLLABLE`); never leave a
   few-pixel rubber band. When it cannot fit, keep the page content as the one
   scroll owner, distribute rows with a uniform gap, and end the first screen
   on a whole-row boundary so no control is sliced by the viewport edge.
   Disable accidental horizontal scrolling and avoid nested scrollables unless
   their handoff is specified.

## Give Every Label a Policy

Configure a label before assigning its first real text:

1. Create it and establish passive or interactive behavior.
2. Set width/height or flex/grid participation and long mode.
3. Set font, color, letter/line spacing, and alignment.
4. Call `lv_label_set_text()` or `lv_label_set_text_fmt()` last.

Bind project fonts explicitly rather than relying on inheritance or widget
defaults. Bind every ordinary text label, including Chinese and dynamic text,
to the appropriate `APP_THEME_FONT_*` role. Bind every `LV_SYMBOL_*` label to
`LV_FONT_DEFAULT`. Do this before assigning text so layout uses the final font
and available glyph set; treat a missing explicit binding as a defect even when
the current theme happens to render correctly.

Select sizing by semantics:

- Keep short numeric values, symbols, and fixed units content-sized. Keep a
  value and inseparable unit in one label, or in a non-wrapping content-sized
  row whose worst-case width is proven.
- Give prose and localized dynamic text a bounded width,
  `LV_LABEL_LONG_WRAP`, and content-driven height.
- Give frequently changing bounded values a stable width based on their
  worst-case formatted text to prevent layout jitter.
- Use scrolling text only where motion is acceptable and the user can wait to
  read it. Do not use `LV_LABEL_LONG_DOT` for required information. Remember
  that a `LV_LABEL_LONG_SCROLL_CIRCULAR` overflow animates forever: the page
  never goes idle, which breaks simulator `wait_idle` coverage and burns
  refresh cycles. Prefer `DOT` for secondary text and size required text to
  its measured worst case instead.
- Measure actual fonts and strings with current LVGL APIs when a tight bound is
  unavoidable. Include Chinese glyphs, fallback fonts, negative values, units,
  and line spacing; do not infer geometry from nominal font size.
- Keep single-line text on one line. A status summary, row value, chip caption,
  or hint that wraps inside a fixed-height row is a defect: condense the copy
  (shorter words, drop separators or the least essential segment, remove
  trailing punctuation) or change the layout (wider container, split the fact
  onto its own row, drop redundant text the selection state already shows).
  Never accept a wrap, ellipsis, or orphaned punctuation for text that can fit
  in one line after refinement; verify the worst-case string renders at one
  line's height in the simulator tree.

## Define Input Ownership

- Let the nearest interactive ancestor own an ordinary tap whenever possible.
  LVGL 9.5 generic objects start clickable, while labels remove `CLICKABLE` in
  their constructor. Inspect each widget class rather than assuming defaults.
- Remove `LV_OBJ_FLAG_CLICKABLE` from passive generic containers and overlays.
  Do not add it to decorative labels or symbols. Remember that
  `lv_obj_remove_style_all()` changes styles only; a styleless generic wrapper
  inside a button remains clickable and can intercept the button's center.
- Add `LV_OBJ_FLAG_EVENT_BUBBLE`, `LV_OBJ_FLAG_GESTURE_BUBBLE`, or scroll-chain
  flags only after naming the intended receiver and axis. They solve different
  propagation problems and are not interchangeable.
- Verify taps and vertical drags that begin over every child label, icon,
  image, and nested control. Confirm that an interactive child neither triggers
  its parent accidentally nor blocks the intended page scroll.
- A control that activates on tap must bind only `LV_EVENT_CLICKED`; never fire
  on `PRESSED`, `PRESS_LOST`, or `RELEASED`. In LVGL 9.5 clickable objects
  inherit `LV_OBJ_FLAG_PRESS_LOCK`, so `CLICKED` still fires when the finger
  slides off before release. Call `app_ui_click_only()` (drops `PRESS_LOCK`) on
  every click-activated button, row, chip, and switch so leaving the control
  sends `PRESS_LOST` and cancels activation; verify with a simulator `drag` that
  starts on the control and ends outside it, asserting no navigation.

## Validate Before Declaring the Layout Fixed

1. Exercise the shortest, longest, missing, loading, error, stale, and
   localized data states. Include large positive/negative numeric formats and
   multi-line Chinese text.
2. Force layout calculation with `lv_obj_update_layout()` in diagnostics or
   tests before reading coordinates. Check child bounds against the parent's
   content rectangle and verify that required text is not clipped.
3. Extend the LVGL host fake to record explicit width, height, long mode, font,
   flags, parentage, and events when the changed contract depends on them.
4. Add tests for scroll direction and gestures beginning over child content,
   not only direct taps on the parent.
5. Drag-verify the scroll policy both ways in the simulator: a scrolling page
   must move (`scroll.y` changes) when dragged over labels, rows, and gaps; a
   pinned page must stay at `scroll.y` 0 with `flags.scrollable` false. Review
   a screenshot of every new or re-laid-out page.
6. Run the narrow host tests and project-required formatting/diff checks.
   Broader sanitizer, firmware-build, and hardware scope follows the
   `AGENTS.md` default workflow.
7. Review in the simulator first: its SDL renderer rasterizes the real theme
   fonts, so glyph fallback, wrapping, clipping, overlap, horizontal drift, and
   scroll handoff are all judgeable from `sim.tree` geometry and screenshots
   without hardware. Read wrap/overflow from `coords` and `styles.line_height`,
   not from a PNG by eye. Use the `ui-review` skill's harness
   (`sim/tools/review_pages.py`) as the gate for a UI change; reserve the real
   display for touch feel, RF, and AMOLED color only.

Report the tested resolutions, data extremes, interaction paths, and any
unexecuted hardware checks. Do not claim that a single nominal screenshot
proves adaptive behavior.

---
name: lvgl-ui-layout
description: Design, implement, review, and repair adaptive LVGL 9.6 user interfaces in MicroTech, with emphasis on Flex/Grid sizing, content-driven containers, per-class text display policy (one-line identifiers, wrapped prose, external input), text measurement and wrapping, clipping and overlap prevention, scroll ownership, touch propagation, and efficient label construction. Use when an LVGL page or widget clips text, ellipsizes required information, wraps a title, leaves an orphaned punctuation tail, overlaps controls, scrolls incorrectly, loses touch gestures over children, relies on absolute coordinates, uses truncating label modes, or needs responsive layout and host-test coverage.
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
   Treat findings as review prompts, not proof of defects. The script does not
   see through project helpers: `BUTTON_CHILD_INPUT` is usually cleared by
   `app_ui_make_passive()`, and `FONT_ROLE_REVIEW`/`MISSING_EXPLICIT_FONT` are
   usually satisfied by the `app_ui_*` label constructors' explicit font
   binding, so confirm against the helper before acting on those two codes.
   `DOT_WITHOUT_WIDTH` means a `DOT` label in this file never received a width,
   so the ellipsis can never appear and the text is silently cut by the parent
   instead; give it a width or flex share, or pick another long mode. Trace
   dynamic font, symbol, and object-helper arguments manually when the script
   reports an indirect role or cannot see the call site.

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
   their handoff is specified. When a page must land exactly on the viewport,
   let fractional grid rows (`LV_GRID_FR`) with
   `LV_GRID_ALIGN_SPACE_BETWEEN` absorb the slack instead of guessed gaps.

## Adapt Within a Fixed Panel

MicroTech drives one 368 x 448 panel at a single density, so raw pixels plus
`lv_pct()`/`LV_GRID_FR()` are the correct units; do not port the demos'
`LV_DPX()`, `LV_HOR_RES`, or size-class tiering without a second density that
needs them. When a layout must still survive a changed size:

- Let content-driven sizing carry the design (`LV_SIZE_CONTENT`,
  `lv_obj_set_flex_grow()`, fractional grid tracks), and cap a content-sized
  card or chart with `lv_obj_set_style_max_*()` when its content is unbounded.
- For geometry Flex/Grid cannot express (a dial needle, canvas annotation, or
  centered overlay), resolve it after `lv_obj_update_layout()` and recompute it
  in an `LV_EVENT_SIZE_CHANGED` handler instead of writing literals.
- Exclude decorative overlays from the flow with `lv_obj_set_ignore_layout()`
  and keep them fixed under scroll with `lv_obj_set_floating()`, then position
  by alignment. Confirm they do not intercept input meant for the content.

See [references/lvgl9-layout-patterns.md](references/lvgl9-layout-patterns.md)
for the concrete patterns.

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

Select sizing by text class. Pick the class first; the class decides the long
mode, not the other way round.

**T1: fixed-shape labels.** Page titles, section headings, row titles, status
words, chip and button captions, values with units. The app authors this text,
so it must render as one line: bounded width (or content width) plus
`LV_LABEL_LONG_WRAP` with a content-driven height. When it does not fit,
condense the copy first (drop separators, the least essential segment, or
trailing punctuation) and only then change the layout (wider container, split
the fact onto its own row, delete what the selection state already shows).
Never "fix" a T1 overflow by switching to `DOT` or by squeezing a sibling.
Verification: the simulator tree reports `len(lines) == 1`.

**T2: authored prose.** Explanations, warnings, empty states, sheet messages,
`app_ui_add_body_label` bodies. Give it its own region: `LV_PCT(100)` width,
`LV_LABEL_LONG_WRAP`, `LV_SIZE_CONTENT` height, and a page that can scroll or
that fits its worst case. A multi-line paragraph must not end on a 1–2 cluster
tail (a lone character or punctuation mark): widen the label by 8–24 px inside
its own layout first, then shorten the copy, then split it into two paragraphs.
For self-wrapped prose these are all one knob — `pad_left`/`pad_right` — because
`app_ui_wrap_prose()` breaks lines from `lv_obj_get_content_width(label)`.
Never hand-insert `\n` to fake a break; `app_ui_set_prose_text()` is the one
sanctioned wrapper because it applies the 行首/行末禁则 sets
(`layers/apps/common/src/app_ui_text.c`). LVGL's own wrap has no kinsoku rules:
it breaks at `LV_TXT_BREAK_CHARS` and anywhere inside CJK, so free-form prose
that matters must go through that helper.

**T3: external input.** SSID (≤32 B), recording file names (≤64 B), weather
city/alert title/body, app `display_name`, mount paths. Length is not yours to
choose, so pick the treatment by position:

| Position | Treatment | Why |
| --- | --- | --- |
| Primary identifier of a list row (SSID, file name, alert title) | Row height `LV_SIZE_CONTENT` with `min_height APP_UI_ROW_HEIGHT`; identifier `LV_LABEL_LONG_WRAP` on the full row width | The user picks from it and must read it whole |
| Passive header identifier (weather city) | `LV_LABEL_LONG_SCROLL_CIRCULAR` | Locked decision, asserted in `tests/integration/test_cross_layer.c` |
| Value in a compact summary row (settings "网络" SSID, Bluetooth name) | `LV_LABEL_LONG_DOT` allowed, but only if the complete value is readable on the owning detail page or confirm sheet | The summary is not the decision surface |
| Unbounded block text (alert body and advice) | Own `WRAP` region plus `app_ui_set_prose_text()`; overlong text is flagged by `content_truncated` | Already implemented in `weather_alert_pages.c` |

Two hard constraints close this class. Never cap required text with
`lv_label_set_max_lines()`: in `WRAP` mode it only clamps the computed height
and adds no ellipsis, which is silent clipping. A `DOT` label is only
acceptable when its full value is reachable in one step; an ellipsis on a
primary identifier is a defect, not a style choice.

The rest of the policy:

- Keep short numeric values, symbols, and fixed units content-sized. Keep a
  value and inseparable unit in one label, or in a non-wrapping content-sized
  row whose worst-case width is proven.
- Give frequently changing bounded values a stable width based on their
  worst-case formatted text to prevent layout jitter.
- `LV_LABEL_LONG_SCROLL_CIRCULAR` animates forever: the page never goes idle,
  which breaks simulator `wait_idle` coverage and burns refresh cycles. Reserve
  it for a single prominent header or status label, never for repeated list
  rows. A *locked product decision* to keep long text fully readable is a
  deliberate exception: MicroTech's weather header city and status stay
  `SCROLL_CIRCULAR` and are asserted in `tests/integration/test_cross_layer.c`;
  do not downgrade them without updating that contract. Treat the no-idle cost
  as a review flag, not an automatic edit.
- `app_ui` row titles and summaries still use `DOT`. They are the target of this
  policy, not an example of it: change them together with their row height the
  next time you touch those rows.
- Measure actual fonts and strings with `lv_text_get_size()` and
  `lv_font_get_line_height()` when a tight bound is unavoidable. Include
  Chinese glyphs, fallback fonts, negative values, units, and line spacing; do
  not infer geometry from nominal font size.
- Read the fit verdict from the simulator, not from the box height: `sim.tree`
  reports `long_mode`, `max_lines`, `line_space` and a `lines` array with each
  rendered line's text and pixel width. A `WRAP` label clipped by a fixed height
  still reports its full `text`, so `expect_visible` alone can never prove it
  fits — the `clipped text`, `clipped by parent` and `truncated text` lints are
  the detectors.

## Define Input Ownership

- Let the nearest interactive ancestor own an ordinary tap whenever possible.
  LVGL 9.6 generic objects start clickable, while labels remove `CLICKABLE` in
  their constructor. Inspect each widget class rather than assuming defaults.
- Remove `LV_OBJ_FLAG_CLICKABLE` from passive generic containers and overlays.
  Do not add it to decorative labels or symbols. Remember that
  `lv_obj_remove_style_all()` changes styles only; a styleless generic wrapper
  inside a button remains clickable and can intercept the button's center.
- Add `LV_OBJ_FLAG_EVENT_BUBBLE`, `LV_OBJ_FLAG_GESTURE_BUBBLE`, or scroll-chain
  flags only after naming the intended receiver and axis. They solve different
  propagation problems and are not interchangeable.
- Verify taps and vertical drags that begin over every child label, icon,
  image, and nested control. Confirm that an interactive child neither
  triggers its parent accidentally nor blocks the intended page scroll.
- A row container that forwards taps to an inner switch needs three
  coordinated pieces: make the row `CLICKABLE` (drop `PRESS_LOCK`), toggle
  the switch on the row's `CLICKED`, and emit `LV_EVENT_VALUE_CHANGED`
  explicitly because a programmatic state flip never fires it; guard the
  handler with `lv_event_get_target(event) !=
  lv_event_get_current_target(event)`, since 9.6 propagates a child's
  `CLICKED` up to clickable ancestors and an unguarded forward double-toggles
  a direct switch tap. There is no `lv_obj_toggle()` in LVGL 9.6; use
  `lv_obj_has_state()` with add/remove.
- An `lv_async_call` cannot be cancelled (no `lv_async_call_delete` in LVGL
  9.6), so deferred work — such as rebuilding a list from inside a clicked
  row's own callback — must re-validate its page root in the callback and
  no-op when the page is gone.
- A control that activates on tap must bind only `LV_EVENT_CLICKED`; never fire
  on `PRESSED`, `PRESS_LOST`, or `RELEASED`. In LVGL 9.6 clickable objects
  inherit `LV_OBJ_FLAG_PRESS_LOCK`, so `CLICKED` still fires when the finger
  slides off before release. Call `app_ui_click_only()` (drops `PRESS_LOCK`) on
  every click-activated button, row, chip, and switch so leaving the control
  sends `PRESS_LOST` and cancels activation; verify with a simulator `drag` that
  starts on the control and ends outside it, asserting no navigation. App
  Manager and other code that cannot include `app_ui` must drop the flag
  directly with `lv_obj_set_press_lock(obj, false)`; every click-activated
  control in the app_manager system layer (control-center tiles and power
  buttons, task-switcher buttons) needs the same treatment.

## Validate Before Declaring the Layout Fixed

1. Force layout calculation with `lv_obj_update_layout()` in diagnostics or
   tests before reading coordinates. Check child bounds against the parent's
   content rectangle and verify that required text is not clipped.
2. Extend the LVGL host fake to record explicit width, height, long mode, font,
   flags, parentage, and events when the changed contract depends on them, and
   let host tests drive gestures that begin over child content — labels, rows,
   gaps — not only direct taps on the parent.
3. Run the narrow host tests and project-required formatting/diff checks.
   Broader sanitizer, firmware-build, and hardware scope follows the
   `AGENTS.md` default workflow.
4. Simulator-side validation — worst-case data states (shortest, longest,
   missing, loading, error; signed numeric formats; multi-line Chinese), the
   scroll policy checked both ways by dragging (a scrolling page must move
   `scroll.y`; a pinned page must stay at 0 with `flags.scrollable` false),
   geometry judged from `coords` and `styles.line_height` rather than a PNG,
   and the release gate — are specified once, by the `ui-review` skill at its
   assigned tier. Reserve the real display for touch feel, RF, and AMOLED
   color only.

Report the tested resolutions, data extremes, interaction paths, and any
unexecuted hardware checks. Do not claim that a single nominal screenshot
proves adaptive behavior.

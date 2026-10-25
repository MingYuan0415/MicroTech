# LVGL 9 Layout Patterns

Use these patterns after confirming the APIs in the project's locked LVGL 9.6
headers.

## Contents

- [Geometry Budget](#geometry-budget)
- [Adaptive Row](#adaptive-row)
- [Content-Sized Prose](#content-sized-prose)
- [Text Fit Classes](#text-fit-classes)
- [Cap Content-Sized Containers](#cap-content-sized-containers)
- [Inseparable Values and Units](#inseparable-values-and-units)
- [One-Screen Fit vs Scroll](#one-screen-fit-vs-scroll)
- [Fill the Viewport with the Grid](#fill-the-viewport-with-the-grid)
- [Reflow by Size Class](#reflow-by-size-class)
- [Measured Overlays and SIZE_CHANGED](#measured-overlays-and-size_changed)
- [Decor Overlays Out of Flow](#decor-overlays-out-of-flow)
- [Align an Axis to Its Plot](#align-an-axis-to-its-plot)
- [Density and Units](#density-and-units)
- [Input and Scroll Topology](#input-and-scroll-topology)
- [Acceptable Fixed Geometry](#acceptable-fixed-geometry)

## Geometry Budget

Use LVGL's computed content dimensions after a layout update whenever possible:

```text
inner_width = parent_width - left/right border - left/right padding
row_required = fixed_children + gaps + flexible_minimum
text_height = font_line_height * line_count + line_spacing * (line_count - 1)
container_height = text_height + top/bottom padding + top/bottom border
```

`lv_obj_get_content_width()` and `lv_obj_get_content_height()` already express
the usable content area. For pre-layout reasoning, include all style parts and
states that can change padding or border width.

Measure tight text bounds with `lv_text_get_size()` and obtain line height with
`lv_font_get_line_height()`. Pass the actual font, letter spacing, line spacing,
maximum width, and text flags. A font's nominal name or point size is not a
layout measurement. Missing CJK glyphs can change fallback metrics, so verify
the real font chain on hardware.

Resolve the intended font before text assignment, directly or through the
owning helper; label policy lives in the parent skill. Verify glyph coverage
rather than assuming widget defaults.

## Adaptive Row

Use a row for a fixed icon, flexible wrapped text, and fixed action:

```c
lv_obj_set_width(row, LV_PCT(100));
lv_obj_set_height(row, LV_SIZE_CONTENT);
lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

lv_obj_set_size(icon, icon_size, icon_size);

lv_obj_set_width(text, 0);
lv_obj_set_height(text, LV_SIZE_CONTENT);
lv_obj_set_flex_grow(text, 1);
lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
lv_obj_set_style_text_font(text, font, 0);
lv_obj_set_style_text_color(text, color, 0);
lv_label_set_text(text, value);

lv_obj_set_size(action, touch_size, touch_size);
```

Include row padding, column gap, icon width, and action width when proving the
text's minimum useful width. If the row itself is content-sized horizontally,
replace the percentage/flex relationship with a resolved parent track.

## Content-Sized Prose

For a paragraph or alert body, resolve width and allow height to follow the
wrapped text:

```c
lv_obj_set_width(label, LV_PCT(100));
lv_obj_set_height(label, LV_SIZE_CONTENT);
lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
lv_obj_set_style_text_font(label, body_font, 0);
lv_obj_set_style_text_color(label, body_color, 0);
lv_obj_set_style_text_line_space(label, line_space, 0);
lv_label_set_text(label, body);
```

Do not set a fixed paragraph height unless the product explicitly defines a
scrolling viewport. If it does, make that viewport the scroll owner and keep
the label content-sized inside it.

## Text Fit Classes

Use the T1/T2/T3 policy in `SKILL.md`; these are implementation examples,
not additional rules. Adapt them to the actual page contract.

### T1 pattern: one-line identity

For a title or value intended to remain one line, size it for the worst case:

```c
lv_obj_set_width(title, LV_PCT(100));      /* or LV_SIZE_CONTENT */
lv_obj_set_height(title, LV_SIZE_CONTENT);
lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
lv_obj_set_style_text_font(title, title_font, 0);
lv_label_set_text(title, copy);
```

```c
lv_point_t size;
lv_text_get_size(&size, worst_case, title_font, letter_space, 0,
                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
if (size.x > lv_obj_get_content_width(title))
{
    /* condense the copy, widen the container, or split the fact */
}
```

In the simulator, the same verdict is `len(lines) == 1` for the label node.

### T2 pattern: authored prose in its own region

```c
lv_obj_set_width(body, LV_PCT(100));
lv_obj_set_height(body, LV_SIZE_CONTENT);
lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
lv_obj_set_style_text_font(body, body_font, 0);
lv_obj_set_style_text_line_space(body, 5, 0);
char prose[WEATHER_SERVICE_ALERT_TEXT_BYTES + 128U];
app_ui_set_prose_text(body, text, prose, sizeof(prose));
```

`app_ui_set_prose_text()` measures the label's own content width, so the label
must already have its bounded width and `WRAP` mode. It needs a scratch buffer
sized for the worst field plus slack; `weather_alert_pages.c` uses
`WEATHER_SERVICE_ALERT_TEXT_BYTES + 128U` because the wrapper inserts `\n`
characters. When wrapping cannot produce output, the helper assigns the
original text, falling back to plain LVGL wrapping without kinsoku rules.

Adjusting the break points means changing the measured width: `pad_left` and
`pad_right` on the label move the wrap boundary by the same amount. Do not
insert layout-dependent breaks by hand; deliberate paragraph boundaries are
separate from automatic wrapping.

The optional `balanced_prose` spec selector checks short tails and punctuation;
its coverage and limitations are described by `ui-review` and `sim/README.md`.

### T3 pattern: external identifiers own their row

An SSID, file name, or alert title has an unknown length, so the row must yield
vertical space to it instead of clipping:

```c
lv_obj_set_width(row, LV_PCT(100));
lv_obj_set_height(row, LV_SIZE_CONTENT);
lv_obj_set_style_min_height(row, APP_UI_ROW_HEIGHT, 0);
lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                      LV_FLEX_ALIGN_CENTER);

lv_obj_set_width(name, LV_PCT(100));
lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
lv_obj_set_style_text_font(name, small_font, 0);
lv_label_set_text(name, record->ssid);
```

Put the secondary line (security, RSSI, duration) below the identifier with
`LV_PCT(100)` width so both share the row's inner width. A fixed row height
plus `DOT` is the old pattern; do not copy it for external identifiers.

Keep `DOT` only where the value is a compact summary whose complete text is
readable on the owning detail page, and never cap required text with
`lv_label_set_max_lines()`: in `WRAP` mode it clamps the computed height without
adding an ellipsis, so the text is silently cut.

## Cap Content-Sized Containers

A `LV_SIZE_CONTENT` container that holds dynamic text, a chart, or user data
can grow past its intended share of the page. Cap the axis that must not run
away, and treat the value as a documented contract rather than a guess:

```c
lv_obj_set_style_max_height(card, 300, 0);   /* no taller than its slot */
lv_obj_set_style_max_width(tile, 160, 0);    /* keep a two-up grid two-up */
```

Cap a card that should not exceed a fraction of the viewport, a chart that
would otherwise stretch on a short page, or a tile sized from unbounded data.
A cap is required only where the content is unbounded; do not add one to a
container whose maximum is already implied by the layout.

## Inseparable Values and Units

Prefer a single label for values that must read atomically:

```c
lv_obj_set_width(value, LV_SIZE_CONTENT);
lv_obj_set_style_text_font(value, value_font, 0);
lv_obj_set_style_text_color(value, value_color, 0);
lv_label_set_text_fmt(value, "%d °C", temperature);
```

If value and unit require different fonts, place two content-sized labels in a
content-sized non-wrapping row. Measure the worst-case pair and do not let the
row shrink below that width.

A value-with-unit normally stays together on one line without ellipsis.
Size the label from the widest formatted value rather than the current one, so a
reading growing from `9 °C` to `-18 °C` or `100 %` does not push the unit onto a
second line:

```c
lv_point_t size;
lv_text_get_size(&size, "-18 °C", value_font, letter_space, 0,
                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
lv_obj_set_width(value, size.x);
lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
```

`home_app.c` already pins the battery percentage this way by measuring `"100%"`.
Do not use `DOT` here: a truncated unit (`1...`) reads as a different value.

## One-Screen Fit vs Scroll

Decide scrolling per page from a measured vertical budget, not habit. On the
368 x 448 panel, derive the usable content viewport from the current page
helper, header, safe area, and padding rather than copying an old pixel budget.

```text
scroll_overflow = last_child_bottom + bottom_padding - viewport_bottom
```

Bottom padding counts toward the scrollable extent: a page whose children end
exactly at the viewport still scrolls by its `pad_bottom`.

- When every control fits and the composition looks intentional, disable
  scrolling explicitly:

```c
lv_obj_set_scroll_dir(content, LV_DIR_NONE);
lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
```

  A page that is a few pixels over budget must not stay a 6 px rubber band:
  tighten gaps and padding to land on the exact viewport height, or accept
  scrolling as a visible design decision.

- When content cannot fit, keep the page content as the single scroll owner
  (the `app_ui` header-page default), distribute rows with a uniform gap, and
  prefer whole-row first-screen boundaries where practical. Variable-height
  content may cross the fold; verify the remainder is reachable by dragging.

On a fixed panel an exact one-screen grid may use explicit pixel budgets
(for example 12 + 3 x 136 + 2 x 8 + 12 = 448); keep the invariant in named
layout constants instead of scattering magic numbers.

## Fill the Viewport with the Grid

When a page must end exactly on the viewport edge, let fractional spacer rows
absorb the leftover instead of guessing gaps between children:

```c
static const int32_t row_dsc[] = {
    LV_GRID_CONTENT,   /* Header block */
    LV_GRID_FR(1),     /* Slack above */
    LV_GRID_CONTENT,   /* Primary block */
    LV_GRID_FR(3),     /* Weighted slack */
    LV_GRID_CONTENT,   /* Actions */
    LV_GRID_TEMPLATE_LAST
};
lv_obj_set_grid_dsc_array(page, NULL, row_dsc);
lv_obj_set_style_grid_row_align(page, LV_GRID_ALIGN_SPACE_BETWEEN, 0);
```

`LV_GRID_FR(n)` rows share the remaining height by weight; `SPACE_BETWEEN`
pushes the content-sized rows to the edges. This lands on the exact viewport
for any content height that fits, and unlike hand-added pixel gaps it degrades
gracefully when a font or string changes. Give the page one grid, not a stack
of absolute offsets. If the content can overflow, use
[One-Screen Fit vs Scroll](#one-screen-fit-vs-scroll) instead and keep the
overflow as a deliberate scroll.

## Reflow by Size Class

MicroTech drives one fixed 368 x 448 panel, so most pages need no reflow. When a
layout must serve more than one width (a second panel, a split view, or a
future density), change the structure in named steps instead of scaling
coordinates:

```c
lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
...
lv_obj_set_flex_in_new_track(card, true); /* start the next flex line here */
```

`lv_obj_set_flex_in_new_track()` starts a new flex line before that child, so a
single `LV_FLEX_FLOW_ROW` container lays out as one row when wide and several
rows when narrow without declaring wrap; pick the `new_track` calls and the
whole grid descriptor set per size class, and switch fonts and icon sizes as
discrete buckets. Do not interpolate a font size or drag every coordinate.
Reach for `LV_DPX()` or a `disp_size`-style tier only per
[Density and Units](#density-and-units).

## Measured Overlays and SIZE_CHANGED

Geometry that must be drawn relative to a resolved box — a dial needle, a
canvas annotation, a centered overlay — cannot be expressed with Flex or Grid.
Resolve it after layout and recompute it on size change instead of hardcoding:

```c
lv_obj_update_layout(scale);
lv_obj_add_event_cb(scale, size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);
```

```c
static void size_changed_cb(lv_event_t *e)
{
    lv_obj_t *scale = lv_event_get_target(e);
    int32_t half = LV_MIN(lv_obj_get_width(scale),
                          lv_obj_get_height(scale)) / 2;
    lv_obj_align(needle, LV_ALIGN_TOP_LEFT, half, half);
}
```

Base the position on the current content rectangle and the measured child
size, never on literals that suit one size. Call `lv_obj_update_layout()`
before reading geometry outside the normal layout pass (diagnostics, tests, or
a draw callback that must place a child).

## Decor Overlays Out of Flow

An overlay that should not move with scrolling or contribute to its parent's
size — a floating action button, a pull hint, a scrim, a badge — is excluded
explicitly rather than positioned around the flow:

```c
lv_obj_set_ignore_layout(overlay, true); /* parent layout skips it */
lv_obj_set_floating(overlay, true);      /* parent scroll does not move it */
lv_obj_align(overlay, LV_ALIGN_BOTTOM_RIGHT, -pad, -pad);
```

`ignore_layout` removes the object from its parent's Flex/Grid, and `floating`
keeps it fixed while the parent scrolls. Alignment, not coordinates, owns its
position. Verify the overlay cannot swallow input meant for the content
beneath it; see [Input and Scroll Topology](#input-and-scroll-topology).

## Align an Axis to Its Plot

A chart or scale and its axis labels share a plotting region; derive the label
padding from the resolved plot so they line up at any width:

```c
lv_obj_set_style_pad_hor(hor_axis,
                         lv_obj_get_style_pad_left(plot, LV_PART_MAIN), 0);
lv_obj_set_style_pad_ver(ver_axis,
                         lv_obj_get_style_pad_top(plot, LV_PART_MAIN), 0);
```

To show more data than fits, give the plot `lv_pct(200)` inside a wrapper that
owns `LV_DIR_HOR` scroll and no other axis, and keep the axis on the wrapper's
track so it tracks the plot rather than the viewport.

## Density and Units

This board is a single 368 x 448 panel at fixed density, so raw pixels are the
correct unit and `LV_DPX()` or `LV_HOR_RES`-based tiering are not needed. Do
not copy the demos' density code in blindly. Introduce `LV_DPX()` (and a small
size-class helper) only when a second density or panel width actually exists,
and then use it for gaps, radii, and control sizes — never to proportionally
scale a whole design. Content-driven sizing (`LV_SIZE_CONTENT`,
`lv_obj_set_flex_grow()`, `LV_GRID_FR()`) carries the rest without a density
factor.

## Input and Scroll Topology

In the locked LVGL 9.6 implementation:

- A generic `lv_obj` constructor adds `CLICKABLE`, `SCROLLABLE`, scroll-chain,
  and gesture-bubble flags.
- The label constructor removes `CLICKABLE`; do not re-add it for decoration.
- `EVENT_BUBBLE` forwards ordinary events to a parent.
- `GESTURE_BUBBLE` forwards recognized gestures to a parent.
- `SCROLL_CHAIN_HOR` and `SCROLL_CHAIN_VER` let a scrollable child hand
  remaining movement to a scrollable ancestor on the named axis.
- LVGL 9.6 resolves the scroll target by hit-testing the press point for the
  deepest `CLICKABLE` object and then walking up ancestors only: a
  non-scrollable object without the chain bit for the current direction stops
  the search, and the walk never descends. Keep the scroll owner `CLICKABLE`
  and `SCROLLABLE` (otherwise presses on its non-clickable descendants fall
  through past it to the screen), and never clear the chain bits from passive
  wrappers between an interactive child and the scroll owner.

For a passive generic surface:

```c
lv_obj_remove_flag(surface, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
```

### Passive wrapper inside a button

The following layout creates a dead touch region even though the wrapper has
no visible style:

```c
lv_obj_t *button = lv_button_create(parent);
lv_obj_add_event_cb(button, button_clicked_cb, LV_EVENT_CLICKED, NULL);

lv_obj_t *content = lv_obj_create(button);
lv_obj_remove_style_all(content);
lv_obj_set_width(content, 0);
lv_obj_set_flex_grow(content, 1);

lv_obj_t *label = lv_label_create(content);
lv_obj_set_style_text_font(label, body_font, 0);
lv_obj_set_style_text_color(label, text_color, 0);
lv_label_set_text(label, "Action title");
```

`lv_obj_remove_style_all()` removes styles, not object flags. The generic
`content` object therefore keeps LVGL 9's default `CLICKABLE` flag, becomes the
input target over its area, and consumes the click without a callback or
`EVENT_BUBBLE`. A label placed directly on the button behaves differently:
the label constructor removes `CLICKABLE`, so the button remains the input
target beneath it.

Make a purely structural wrapper passive explicitly:

```c
lv_obj_remove_flag(content,
                   LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
```

When diagnosing a partially dead button, inspect every generic child between
the visible label and interactive ancestor. Remove `CLICKABLE` from structural
wrappers. Use event bubbling only when a child is intentionally an input target
and the parent also needs the event. This is an input-target topology defect,
not evidence that LVGL 9 labels are clickable by default.

For an interactive child inside a vertically scrolling page, decide separately
whether it should receive taps, recognize gestures, scroll itself, and hand
vertical movement to the page. Add only the flags required by that topology.
Test dragging from the child's rendered text/icon, not just its empty padding.

### Flex and widget pitfalls

- `LV_FLEX_FLOW_ROW_WRAP` never wraps children sized as `width 0 +
  lv_obj_set_flex_grow()`: their base size is zero, so all of them share one
  row. Give grid cards an explicit width and count the column gap in the
  budget.
- A `width 0 + flex_grow` button inside a non-flex container collapses to
  zero width and becomes an invisible, unclickable control. Grow children need
  a flex row parent.
- Do not call `lv_obj_center()` or `lv_obj_align()` on a flex child; flex
  placement owns its position. Center through the parent's alignment or a
  dedicated wrapper.
- Widget APIs are not interchangeable: `lv_label_set_text()` on an
  `lv_button` corrupts memory in release builds where class asserts are
  compiled out. A button's caption is its child label; reach it through
  `lv_obj_get_child(button, 0)`.

## Acceptable Fixed Geometry

Fixed geometry is valid when tied to a stable contract, such as a 44 px touch
target, a source bitmap's exact size, a chart plotting region, or a height
derived from `lv_font_get_line_height()`. It is suspect when several sibling
objects require coordinated `x`, `y`, width, and height literals to avoid
overlap.

When absolute placement is required for a plot annotation or canvas overlay,
derive it from the current parent content rectangle and measured child size,
then recompute it on size/style changes. Keep ordinary page composition in
Flex/Grid.

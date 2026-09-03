# LVGL 9 Layout Patterns

Use these patterns after confirming the APIs in the project's locked LVGL 9.5
headers.

## Contents

- [Geometry Budget](#geometry-budget)
- [Adaptive Row](#adaptive-row)
- [Content-Sized Prose](#content-sized-prose)
- [Inseparable Values and Units](#inseparable-values-and-units)
- [One-Screen Fit vs Scroll](#one-screen-fit-vs-scroll)
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

For MicroTech, explicitly bind every ordinary label to its semantic
`APP_THEME_FONT_*` font before assigning text. Explicitly bind every label that
renders an `LV_SYMBOL_*` to `LV_FONT_DEFAULT`. Do not rely on inherited theme
fonts: an inherited font can lack Chinese or symbol glyphs and render boxes,
and changing it after text assignment causes an avoidable second layout pass.

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

## One-Screen Fit vs Scroll

Decide scrolling per page from a measured vertical budget, not habit. On the
368 x 448 panel the content viewport is 448 px for a headerless page and
384 px below the 64 px header.

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
  size rows so the first screen ends on a whole-row boundary. No control may
  be sliced by the viewport edge; the remainder is revealed by dragging.

On a fixed panel an exact one-screen grid may use explicit pixel budgets
(for example 12 + 3 x 136 + 2 x 8 + 12 = 448); keep the invariant in named
layout constants instead of scattering magic numbers.

## Input and Scroll Topology

In the locked LVGL 9.5 implementation:

- A generic `lv_obj` constructor adds `CLICKABLE`, `SCROLLABLE`, scroll-chain,
  and gesture-bubble flags.
- The label constructor removes `CLICKABLE`; do not re-add it for decoration.
- `EVENT_BUBBLE` forwards ordinary events to a parent.
- `GESTURE_BUBBLE` forwards recognized gestures to a parent.
- `SCROLL_CHAIN_HOR` and `SCROLL_CHAIN_VER` let a scrollable child hand
  remaining movement to a scrollable ancestor on the named axis.
- LVGL 9.5 resolves the scroll target by hit-testing the press point for the
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

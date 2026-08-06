# LVGL 9 Layout Patterns

Use these patterns after confirming the APIs in the project's locked LVGL
headers. They map useful LVGL 7.11 ideas to LVGL 9.5 without carrying forward
obsolete widget defaults.

## Contents

- [LVGL 7 to LVGL 9 Mapping](#lvgl-7-to-lvgl-9-mapping)
- [Geometry Budget](#geometry-budget)
- [Adaptive Row](#adaptive-row)
- [Content-Sized Prose](#content-sized-prose)
- [Inseparable Values and Units](#inseparable-values-and-units)
- [Input and Scroll Topology](#input-and-scroll-topology)
- [Acceptable Fixed Geometry](#acceptable-fixed-geometry)
- [Review Sources](#review-sources)

## LVGL 7 to LVGL 9 Mapping

| LVGL 7 idea | LVGL 9 pattern |
| --- | --- |
| `lv_cont` row/column layout | Generic `lv_obj` with Flex flow and alignment |
| `lv_cont` grid-like layout | Generic `lv_obj` with Grid descriptors/cells |
| FIT to children | `LV_SIZE_CONTENT` on the content-driven axis |
| Child fills remaining area | Zero base size plus `lv_obj_set_flex_grow()` or `LV_GRID_FR()` |
| Manual child coordinates | Flex/Grid placement or `lv_obj_align()` for a true overlay |
| v7 click assumptions | Inspect the LVGL 9 widget constructor and explicit object flags |

Do not use percentages on the same axis where the parent is
`LV_SIZE_CONTENT`. Resolve one side of the relationship first.

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
lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
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
lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
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

## Input and Scroll Topology

In the locked LVGL 9.5 implementation:

- A generic `lv_obj` constructor adds `CLICKABLE`, `SCROLLABLE`, scroll-chain,
  and gesture-bubble flags.
- The label constructor removes `CLICKABLE`; do not re-add it for decoration.
- `EVENT_BUBBLE` forwards ordinary events to a parent.
- `GESTURE_BUBBLE` forwards recognized gestures to a parent.
- `SCROLL_CHAIN_HOR` and `SCROLL_CHAIN_VER` let a scrollable child hand
  remaining movement to a scrollable ancestor on the named axis.

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

## Review Sources

The workflow also incorporates general ideas reviewed from these public skills:

- `font-measurement` from Even Realities: measure actual text, subtract border
  and padding before wrapping, and account for missing-glyph behavior. Its
  display and font constants do not apply to MicroTech.
- `ux-css-layout` from VS Code: distinguish fixed and flexible children, model
  scroll ownership, and treat text overflow as an explicit policy. Its CSS
  ellipsis recommendation does not override this skill's rule that required
  embedded-display information must remain fully readable.
- `specialized` from `zephyr-agent-skills`: validate GUI behavior with the
  actual embedded rendering environment. Its Zephyr configuration examples do
  not apply to this ESP-IDF project.

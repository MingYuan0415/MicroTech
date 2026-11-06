# Feedback Patterns

## Press

- Highlight on press-in, not on release.
- Use a colour/opacity change or a very small scale if the style permits.
- Do not move surrounding content or change layout bounds.
- Release must not produce a second, competing animation.
- If the action is destructive, the press feedback must not look like success.

## Selection

- Selected state needs a persistent visual difference, not only a temporary
  ripple.
- Use the profile's `surface_pressed` or accent tint plus an outline; do not
  invent a new colour.
- Keep selected and focused distinct in profiles that support focus.
- Never use hue alone; pair with text, icon or shape.

## Loading

- Preserve the geometry of the content being loaded.
- Prefer a skeleton or progress indicator that matches the final composition.
- Do not use a full-screen spinner when only one row is loading.
- Do not animate indefinitely when the operation can fail or be cancelled.

## Empty and Error

- Empty state: what is missing, why it matters, and one action.
- Error state: what happened, what was preserved, and one recovery action.
- Do not animate an error into view repeatedly.
- Do not use colour alone to distinguish error from warning.

## Sheets and Overlays

- A sheet should enter from the edge that matches its dismissal path.
- Do not animate a sheet's height; translate it inside a clipped region.
- Back/cancel must not be blocked by an in-flight transition.
- A modal that grows into a multi-step flow should have its own navigation
  context, not a replaced page behind it.

## Toasts

- A toast is transient, non-modal feedback: anchor it to the page root with
  `IGNORE_LAYOUT` + `FLOATING`, never insert it into a flex/grid flow.
- Keep one toast per page; a new message replaces the caption and restarts the
  dwell time. A tap must pass through to the content underneath.
- Enter and exit inside the `micro` budget: 140 ms in and 120 ms out with
  opacity plus a short translate; no loop and no layout-property animation.
- Use the shared `app_ui_toast_show()` helper instead of page-local toast
  copies so dismissal, replacement, and teardown behave consistently.

## Lists

- Do not stagger rows on every page entry.
- Reordering or filtering may use a short cross-fade, but keep row geometry
  stable.
- Scroll performance is a layout and draw concern; use `lvgl-ui-layout` and
  `lvgl-integration` rather than adding decorative scroll effects.

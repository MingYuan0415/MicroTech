# Gesture Semantics

## Principles

- A gesture is a control, not a decoration. It needs a discoverable alternative
  when the same action matters on a keypad or non-touch target.
- Feedback starts on touch-down and tracks the finger one-to-one while the
  gesture is active.
- A gesture must be cancellable by moving back or releasing outside the target.
- One region has one primary gesture. Avoid nested tap/drag/back conflicts.
- A gesture that changes a durable value must commit once, at release, with a
  clear state change.

## Common Patterns

| Gesture | Use | Avoid |
| --- | --- | --- |
| Tap | Select, activate, toggle | Double-tap-only actions |
| Long press | Enter selection mode, reveal secondary action | Destructive action without confirmation |
| Horizontal drag | Slider, swipe-to-dismiss, paging | Global navigation that conflicts with scroll |
| Vertical drag | Scroll, sheet dismissal | Dragging a control that also scrolls |
| Pinch | Zoom, only when the content needs it | Decorative scaling |

## MicroTech Boundaries

- Scroll ownership belongs to `lvgl-ui-layout`.
- The bottom-sheet and back behavior belong to the App Manager / shared UI
  layer; do not create a second navigation model.
- A gesture must not steal input from a child control.
- If the target is keypad/encoder-only, every gesture-only action needs an
  explicit button or menu path.

## Motion

- Use the platform's scroll/momentum behavior for scrolling; do not implement a
  custom spring.
- A sheet follows the finger; if released, settle with the target's allowed
  duration, not a spring simulation.
- Interruptible means the new input reads the current visual value, not the
  logical target.
- Do not lock input during a transition.
- Enter and exit along the same path; do not invent a different direction for
  dismissal.

## Verification

Use `ui-review` for tap/drag/back scenarios. For each changed gesture, test:

1. press and release inside;
2. drag off and release (must cancel);
3. drag beyond the boundary (must resist, not jump);
4. rapid repeated activation;
5. back/cancel during an in-flight transition;
6. reopen after dismissal.

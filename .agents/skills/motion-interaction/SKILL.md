---
name: motion-interaction
description: Plan and review press feedback, transitions, gestures and loading-state motion for MicroTech within an embedded resource budget. Use whenever a UI change adds or changes animation, interaction feedback, sheets, page transitions or gesture behavior.
---

# MicroTech Motion and Interaction

Motion is a state signal, not decoration. The target profile sets the budget;
the style profile sets the ambition; the resolved result is the lower of the
two.

## Sources of Truth

- `design-contract` resolves `motion_level`, `max_motion_ms`,
  `max_animated_objects`, `allow_blur`, `allow_shadow` and `allow_gradients`.
- `lvgl-integration` owns the display pipeline, flush handshake and display
  power transitions.
- `lvgl-ui-layout` owns scroll ownership and touch propagation.
- `ui-review` owns simulator interaction evidence and gesture checks.
- `references/inspiration.md` records provenance and scope.

## Workflow

1. Resolve the profile/target pair. Use `select --mode design` for a new
   interaction plan or `select --mode current` when auditing the current UI.
2. Inventory the interaction: trigger, frequency, purpose, what changes on
   screen, and whether the user can interrupt it.
3. Apply the frequency gate in `references/motion-budget.md`.
4. Choose the cheapest mechanism using `references/feedback-patterns.md` and
   `references/gesture-semantics.md`:
   - no animation;
   - a style transition (colour, opacity);
   - `lv_anim` for a short, bounded transform.
5. Write a motion plan with `duration_ms`, `objects`, `property`, `loop`,
   `mechanism` and `trigger`. `property` may be omitted for an instant
   `mechanism: "none"` item.
6. Validate it:

   ```bash
   python3 .agents/skills/motion-interaction/scripts/motion_budget.py \
       --plan <motion-plan.json> --context <context.json> --check
   ```

7. Implement only the approved plan; verify with `ui-review`. State whether the
   evidence is simulator-only or hardware-measured.

## Embedded Rules

- Default to no animation for frequent actions: back, app switching, scrolling,
  keyboard/keypad, list navigation.
- Press feedback must be immediate; a colour/opacity change is usually enough.
- Do not animate layout properties (`width`, `height`, `left`, `top`).
- Do not animate blur, backdrop, shadow, large images, particles or decorative
  loops.
- Do not simulate spring physics on the MCU. If a gesture needs momentum, use
  the platform's scroll behavior, not a custom spring.
- No infinite or looping animation. It burns power, prevents idle, and breaks
  `sim.wait_idle`.
- No scroll-linked choreography or staggered list entrances.
- A transition must not block input or delay the next action.
- If the target profile has no motion budget, all interactions are instant.

## Motion Levels

| Level | Use |
| --- | --- |
| `none` | Unknown/constrained target, high-frequency actions, monochrome/keypad UI |
| `micro` | Press, selection, small state change |
| `standard` | Occasional page/sheet transition on a capable target |
| `rich` | Not a default; only a specific, approved, high-capability case |

The style profile may ask for more; the target cap wins. Record
`MOTION_DOWNGRADED` when this happens.

## Required States

Every interactive control needs a visible pressed state. Toggles, selection and
disabled states must be distinguishable without relying on motion. A loading
state must preserve the geometry of the content it replaces.

## Handoff

Report the resolved motion level, the chosen mechanism per interaction, the
budget check result, and what could not be measured. Never claim a frame rate or
touch latency from a static screenshot.

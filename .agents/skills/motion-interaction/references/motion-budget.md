# Motion Budget

## The Frequency Gate

Ask how often a person triggers the interaction:

| Frequency | Decision |
| --- | --- |
| Hundreds of times per day | No animation; instant state change |
| Tens of times per day | Minimal feedback, well under the profile's `micro` cap |
| Occasional (daily) | Short transition allowed |
| Rare / first-time | Small delight allowed, still within the target cap |

If the answer is uncertain, choose less motion. Deleting an animation is a valid
success.

## Purpose Gate

Name one purpose before adding motion:

- **feedback** — the interface heard the action;
- **state change** — something became selected, enabled or complete;
- **spatial continuity** — the new surface came from where the action was;
- **prevent a jarring cut** — content would otherwise disappear abruptly;
- **explanation** — motion shows how something works;
- **delight** — a rare, first-time moment.

“It looks cool” on a frequent action is a rejection.

## Budget Model

The target profile supplies:

- `max_motion_ms` — wall-clock duration ceiling;
- `max_animated_objects` — how many independent elements may animate at once;
- `allow_blur` / `allow_shadow` / `allow_gradients`;
- `max_image_bytes` / `max_icon_count`.

The style profile supplies `motion_level`. Resolve with:

```text
resolved_level = min(style.motion_level, target.max_motion_level)
max_motion_ms = min(target.max_motion_ms, cap_for(resolved_level))
max_animated_objects = min(target.max_animated_objects, object_cap_for(resolved_level))
```

Hard caps: `micro` = 150 ms / 2 objects; `standard` = 250 ms / 4 objects;
`none` = 0 / 0; `rich` = 300 ms / 4 objects and still requires explicit
approval. The object cap is cumulative across the plan; splitting one
animation into multiple items does not create extra budget.

A `none` target accepts only instant state changes. An instant item uses
`mechanism: "none"`, `duration_ms: 0`, `objects: 0`, and may omit `property`.
A `micro` target accepts short colour/opacity/translate changes. A `standard`
target may accept an occasional page or sheet transition. `rich` is never a
default.

## Allowed and Forbidden

Allowed properties: colour, opacity, short translate, small scale where the
platform supports it without layout.

Forbidden:

- width, height, left, top, layout, blur, shadow, background position,
  filter, clip-path;
- spring physics or simulated bounce;
- particles, fire, water, confetti or ambient loops;
- full-screen image scaling or panning;
- per-frame custom draw or `lv_timer` animation loops;
- scroll-linked choreography;
- staggered list-item entrances;
- infinite or looping animation.

## Mechanism Choice

1. **No animation** — default for high-frequency actions and constrained targets.
2. **Style transition** — colour/opacity changes; cheapest and usually enough.
3. **`lv_anim`** — short bounded transform or fade for an occasional surface.
4. **Never** — a custom physics engine or full-screen compositor.

## Verification

- Static plan: run `motion_budget.py --check`.
- Simulator: use `ui-review` to inspect the actual state sequence and gesture.
- Hardware: frame timing and touch latency are hardware evidence only. Do not
  claim them from a screenshot or a host run.

## Minimal Plan Shape

```json
{
  "profile_id": "paper_light",
  "profile_version": 1,
  "target_id": "panel_240x320_touch_30hz",
  "target_version": 1,
  "items": [
    {
      "id": "primary-press",
      "trigger": "press",
      "property": "opacity",
      "duration_ms": 120,
      "objects": 1,
      "loop": false,
      "mechanism": "style_transition"
    }
  ]
}
```

When present, `profile_version` and `target_version` must match the resolved
context; a stale plan is rejected instead of being validated against a newer
profile or target.

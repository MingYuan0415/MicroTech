---
name: visual-design
description: Plan the visual direction, composition, typography and colour roles for a MicroTech page in any style profile and target profile. Use before creating a new page or restyling an existing one, when the request is about look, hierarchy, density or visual identity rather than lifecycle or layout mechanics.
---

# MicroTech Visual Design

This skill produces a design plan, not LVGL code. The plan is then implemented
through `app-development` and `lvgl-ui-layout`, and reviewed through
`aesthetic-review`.

## Sources of Truth

1. Resolve a design context with `design-contract`. Read
   `doc/design/README.md` and the selected profile/target before planning.
2. Read the page's current contract and the nearest production page for
   integration facts only; do not copy its visual choices as a baseline.
3. Let `lvgl-ui-layout` own fit, wrapping, clipping, Flex/Grid and scroll.
4. Let `app-development` own Page lifecycle, routes and state.
5. `references/inspiration.md` records provenance and scope.

Do not read a palette from the current source tree. `dark_sparse` is one
profile, not the project's universal style.

## Workflow

1. **Read the brief.** Identify the task, audience, use frequency, primary
   action, and what the page must make obvious in the first glance.
2. **Resolve the pair.** Use explicit profile/target IDs. If none are supplied,
   run `design-contract select --mode design`; report `selection.mode`,
   `selection.profile_source` and `selection.target_source` (`explicit`, `env`
   or `fallback`). Never use `active.json` as a design baseline and never infer
   a style from the current source tree.
3. **Write a design plan.** Use `references/direction.md`,
   `references/composition.md` and `references/type-color.md`. The plan must contain:
   - `profile_id`, `profile_version`, `target_id` and `target_version`;
   - semantic role map (surface, ink, accent, danger, focus);
   - type scale and space/radius scale in semantic names, not raw values;
   - composition and visual focus;
   - one product-specific idea, not a template device;
   - required states;
   - motion plan and asset budget, or explicit `none`;
   - what is intentionally left out.
4. **Check the plan before building.** Reject a plan that could be pasted into
   an unrelated product with only a colour swap. If the plan needs a new role
   or a new style axis, change the profile through `design-contract`, not the
   page.
5. **Hand off implementation.** State which parts are `lvgl-ui-layout` concerns
   and which are `visual-design` decisions. Do not place coordinates or raw
   colours in the plan.
6. **Review.** After implementation, use `aesthetic-review` with the same
   profile/target pair. Do not switch profiles to make the review easier.

## Style-Neutral Rules

- A profile may be flat or tonal, light or dark, dense or spacious. None is
  inherently better.
- A strong visual idea is not a decorative effect. It must come from the
  product's subject, data or task.
- Spend boldness in one place; keep the rest quiet.
- Use contrast for hierarchy before adding borders, cards or colour.
- Prefer removing a container over nesting another one.
- Every decorative element must carry information or be removed.
- A page may be expressive in `soft_playful`, but still must meet the universal
  floor and the target budget.

## Required States

Every interactive page plan covers the states it can reach:

- default, pressed, selected, disabled;
- loading, empty, error, success where applicable;
- long text, missing data, stale data;
- back/cancel/return path.

Do not invent a state that the product cannot reach. Mark unavailable states
explicitly.

## Motion and Assets

- Use `motion-interaction` for any transition, press feedback or gesture.
- Choose the cheapest mechanism that communicates the state change; "no
  animation" is a valid design decision.
- Respect `max_motion_ms`, `max_animated_objects`, `allow_shadow`,
  `allow_blur`, `allow_gradients`, `max_image_bytes` and `max_icon_count` from
  the resolved target.
- Do not design around a large hero image, full-screen blur or decoration that
  the target cannot afford.

## Prohibited

- Do not write raw hex, pixel sizes, font files or LVGL style calls in the plan.
- Do not treat the current `dark_sparse` palette as a default.
- Do not assume a panel size, DPI, colour mode or touch input.
- Do not copy a landing-page hero pattern into an Operate surface.
- Do not use the same card stack for every page.
- Do not use emoji as icons or colour as the only semantic signal.
- Do not ask `lvgl-ui-layout` to fix a weak composition with smaller text.

## Handoff

Return the design plan with the resolved profile/target, the universal-floor
checks, the states covered, the motion/asset budget, and the points that remain
unverified until a screenshot is available.

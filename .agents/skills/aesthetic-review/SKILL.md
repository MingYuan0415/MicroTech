---
name: aesthetic-review
description: Review visible MicroTech UI for visual quality in any style profile and target profile. Use when a page needs design critique, visual QA, anti-slop review, hierarchy/typography/colour assessment, or a profile-relative review after a UI change.
---

# MicroTech Aesthetic Review

Review what the user actually sees, against the resolved design context, not
against the current project style.

## Sources of Truth

- `design-contract` resolves the `(profile, target)` pair.
- `doc/design/README.md` and the selected profile/target describe the intended
  visual language and capability budget.
- `ui-review` owns simulator capture, page/state selection, gestures and
  current review snapshots.
- `lvgl-ui-layout` owns geometry, text wrapping, clipping and scroll ownership.
- `style_audit.py` checks the mechanical parts visible in `sim.tree`.
- `references/inspiration.md` records provenance and scope.

## Review Procedure

1. **Resolve the pair.** Never review without a profile/target. If the caller
   is reviewing the current UI, run `design-contract select --mode current`
   and report `selection.mode`, `selection.profile_source` and
   `selection.target_source`; the active pair is the subject, not a baseline.
   For all other reviews, use explicit IDs or `select --mode design`.
2. **Capture evidence.** Use `ui-review` or `python3 tools/agent/mt.py ui` for
   the affected pages and states. Obtain a screenshot and a `sim.tree` dump.
   A source-only review is allowed only with an explicit `UNVERIFIED` marker.
3. **Run the mechanical audit.**

   ```bash
   python3 .agents/skills/aesthetic-review/scripts/style_audit.py \
       --tree <tree.json> --context <context.json> --check
   # spacing is checked by default; use --no-spacing only when computed LVGL
   # padding contains intentional optical values that cannot be compared to
   # profile tokens.
   ```

   The audit checks palette membership, contrast, radius, spacing, touch-target
   size, icon/accent budgets. It is a prompt generator, not a complete design
   review.
4. **Judge the visual result.** Use `references/rubric.md` for the universal
   floor, `references/profile-relative-review.md` for profile-specific
   expectations, and `references/anti-patterns.md` for false-positive checks.
   Look at composition, hierarchy, focus, density, typography, colour use,
   state clarity and copy.
5. **Check profile fit.** If the screenshot/tree does not match the selected
   profile, report `PROFILE_MISMATCH` first. Do not call a light page “too
   bright” because the current profile is dark, or a rounded page “wrong”
   because the current profile is square.
6. **Rank and report.** Use `blocker`, `major`, `minor`. Each finding needs a
   node/region, the violated rule, and a concrete fix. No praise section.

## Universal Floor vs Profile-Relative

**Universal floor** — always report, regardless of style:

- text contrast below the profile's numeric floor;
- touch targets below the resolved minimum (when the target has touch);
- text clipped, overlapping or unreadable;
- missing/ambiguous primary action;
- colour as the only semantic signal;
- disabled, loading, empty or error states missing where reachable;
- destructive action without confirmation or undo;
- emoji used as structural icons;
- inconsistent component vocabulary across the same surface;
- invented data or fake states.

**Profile-relative** — judge against the active profile:

- flat vs tonal vs elevated surfaces;
- border vs shadow vs lightness depth;
- compact vs comfortable vs spacious density;
- square vs soft vs rounded vs pill corners;
- compact vs standard vs editorial type scale;
- none vs sparse vs expressive decoration;
- outline vs filled vs geometric icons;
- cool/neutral/warm colour temperature.

A profile-relative rule can never override the universal floor.

## Report Schema

```json
{
  "profile_id": "...",
  "profile_version": 1,
  "target_id": "...",
  "evidence": {"screenshot": "...", "tree": "..."},
  "findings": [
    {
      "severity": "blocker|major|minor",
      "scope": "universal|profile",
      "rule": "...",
      "node": "...",
      "message": "...",
      "fix": "..."
    }
  ],
  "limitations": ["..."]
}
```

If the report is prose, include the same fields explicitly. State what was not
checked: unreachable states, missing screenshots, hardware-only timing.

## Prohibited

- Do not use the current `dark_sparse` profile as an implicit baseline.
- Do not demand a specific palette, radius, font or decoration unless the
  active profile requires it.
- Do not call a style “bad” because it differs from the previous screen.
- Do not claim a visual pass from a green harness or a compile.
- Do not use `style_audit.py` findings as proof of composition quality.
- Do not review a page against a profile it was not rendered with; report the
  mismatch instead.

---
name: ui-review
description: Review MicroTech visible UI and interaction changes using affected-page simulator states, geometry lint, screenshots, and gestures. Use for layout, rendering, touch, or visible-state changes; pure logic and documentation changes do not automatically need a UI matrix.
---

# UI Review

Use the simulator's real LVGL layout and fonts to check the changed behavior,
not just source structure or a default-state screenshot. General validation and
authorization follow `AGENTS.md`. Commands, tree fields, fixture seeding, and
session operation are maintained in `sim/README.md`.

## Choose Scope Before Running

A full review protocol is not the same as the full repository page set.

| Change | Automated coverage | Human inspection and interactions |
| --- | --- | --- |
| Copy only | Lint and baseline check for touched pages | Read touched states; no unrelated gesture set |
| Visible behavior/state fix | Affected pages and relevant scenarios | Inspect states the fix touches and replay involved interactions |
| New page, new state, local re-layout/restyle | Full protocol for that page; add/update relevant spec entries | Relevant reachable states, gestures, entry and return paths |
| Shared widget/helper | Its actual consumers; full spec if scope cannot be bounded reliably | Representative consumers and changed interactions |
| Global font/theme/viewport or navigation foundation | Cross-page/full coverage as required by impact | Cross-page state and input regressions |
| Pure logic with no visible/input impact | Owning host tests | No automatic screenshot matrix |

`review_pages.py --pages` selects pages, not individual states: automated runs
execute the selected pages' executable spec states. Human screenshot review may
focus on the affected states. Related scenarios can be selected with
`sim/ci/run_ci.sh --scenario <name>`; omit the filter for the full scenario set.
Do not assume every service call change necessarily changes the interface.

## Full Protocol for an Affected Page

1. **State coverage.** Identify relevant reachable empty, loading, populated,
   disabled/busy, error, and transient states. Add deterministic seeds and
   expectations to `sim/ci/review/spec.json` where practical. Record states the
   simulator cannot reproduce as hardware-only limitations; do not invent
   evidence or demand screenshots for impossible combinations.
2. **Geometry and visuals.** Run the affected-page lint/baseline check. Read
   screenshots for composition, color, icons and wrong-state visuals; use tree
   coordinates and rendered lines for exact fit. Inspect long external input,
   signs/units and Chinese prose where they affect the page.
3. **Input.** Exercise changed taps and drags from labels, icons and gaps.
   Ordinary taps dragged off must not activate; scroll owners must move in
   both directions, pinned content must remain pinned, and toggles must persist
   for the lifetime the feature promises. Include navigation away/back where
   state or resource ownership is involved.
4. **Current artifact.** Build once after relevant edits, then review a newly
   launched instance of that binary. A successful build does not update an
   already running process. After a relevant edit, rebuild/restart and repeat
   affected evidence only; unrelated prior results remain reusable.

## What the Harness Does and Does Not Prove

- **Automatic:** rendered-line completeness, some WRAP-height and ancestor
  clipping, observable `...` truncation, control reachability through scroll
  travel, unexpected disabled controls, unknown image IDs, and masked baseline
  differences. Consult `sim/tools/review_pages.py` for exact implementation;
  these checks are not a general proof of all horizontal/vertical clipping.
- **Spec-dependent:** `single_line` and `balanced_prose` match explicit text
  selectors; `expect_visible`, `expect_absent`, and `expect_image` cover only
  their listed expectations. Deliberate summary ellipses use `allow_truncation`.
  A green run cannot prove unlisted label policy, that a selector still matches,
  or that an external row uses content-driven sizing.
- **Manual/source review:** T1/T2/T3 classification (`lvgl-ui-layout`), complete
  identifiers at decision surfaces, appropriate row sizing, meaning preserved
  by copy edits, primary-control placement, fallback visibility, and intentional
  disabled-state styling. Disabled can be correct for busy, ineligible, or
  unavailable actions; add narrow spec allowances with the intended reason
  rather than changing product behavior merely to appease the lint.
- **System layers:** request `sim.tree {include_layers:true}` to inspect top/sys
  overlays. The current page harness walks `children`, not the separate
  `overlays` array, so review these with explicit tree assertions, screenshots,
  and the appropriate system hook/gesture; do not count page lint as coverage.
- **Hardware:** real touch feel, panel timing/color, DMA, radio, I2S and sleep
  require their own evidence. The simulator does not replace those checks or
  authorize hardware operations.

## Run and Interpret

```sh
cmake --build build/sim
python3 sim/tools/review_pages.py --check --pages settings/wifi,settings/bluetooth
sim/ci/run_ci.sh --scenario smoke_pages
```

The review harness normally launches and owns fresh headless sessions with
isolated NVS/SD directories; an occupied port is an error, not permission to
reuse or terminate its owner. For deliberate interactive reuse pass
`--no-launch` after confirming the process/binary and seeded state yourself;
this mode has no per-page process isolation and never stops the external sim.
`--keep-sim` retains the final owned instance and its temporary data for probing;
end it explicitly when finished. Detailed operation lives in `sim/README.md`.

A lint failure requires understanding the violated contract; a baseline diff
requires judging whether pixels changed intentionally. Inspect new captures
before `--update`, update only affected pages, then run `--check`. Do not raise
tolerance, broaden masks, or remove assertions to hide an unintended change.
Masks cover genuinely volatile pixels; lint and explicit assertions still need
to prove their relevant geometry/text. `no_golden` is for surfaces without a
useful stable pixel baseline, not a shortcut around review.

Report selected pages/states and scenarios, important gestures, artifact/session
identity, failures, and unexecuted checks. A green harness plus a single default
screenshot is not evidence for states or paths you did not exercise.

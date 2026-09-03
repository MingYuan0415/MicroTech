---
name: ui-review
description: Adversarial simulator UI review protocol for MicroTech layers/apps. Use after creating or modifying an App, Page, or shared app_ui widget, and before declaring any UI change done, to catch wrapped text, off-screen or unreachable controls, washed-out disabled states, fallback/broken icons, wrong-state visuals, and stale-binary conclusions that structure-only checks and single happy-path screenshots miss. Drives the sim agent through a per-page state matrix with geometry lint and masked PNG baselines.
---

# UI Review

Reviewing a UI change means proving every page looks and behaves right in every
state it can reach, not that a scenario's text assertions pass. The simulator
(SDL, real fonts, real LVGL 9.5 layout) is the primary review surface; hardware
only adds RF, touch feel, and AMOLED color. Treat "host green + one screenshot"
as NOT reviewed.

## Why This Skill Exists (recurring misses)

These shipped as "done" and were caught by a human looking at a screenshot or
doing a gesture, because the review was confirmatory, not adversarial:

- Scroll chain stripped by a passive helper: `tree_assert` text still passed.
- Buttons firing on `PRESS_LOST`: only a press-and-drag-off gesture reveals it.
- Wrapped hub summary, wrapped storage value, wrapped PM chip, orphaned `。`:
  present in the screenshot, missed by eye; a `coords.h` vs `line_height` check
  catches them mechanically.
- Oversized slider knob, reviewed against a stale binary (ninja did not rebuild
  the edited file) and by eyeballing the PNG instead of measuring.
- Broken-image glyph, off-center no-data text, English city after a payload
  clobber: all lived in states that were never screenshotted.
- Danger rows washed-out by `LV_STATE_DISABLED`, and a control buried below a
  populated list so it was unreachable: visible in the first screenshot.

The common failure is trusting structure assertions and a single default-state
screenshot. Fix it with a state matrix, geometry lint, gesture tests, and a
verified artifact.

## The Review Bar (all four, per changed page)

1. **State matrix.** Enumerate and screenshot every state the page can show:
   empty/no-data, loading/scanning, populated, disabled/ineligible, error, and
   interactive-transient (pressed, two-step armed, pairing passkey). A bug in an
   un-screenshotted state is a bug you shipped.
2. **Geometry lint.** Do not eyeball; read it from `sim.tree`. Run
   `sim/tools/review_pages.py` (see below) and drive it clean.
3. **Gesture tests.** Exercise input the way a finger does, via the agent:
   press-and-drag-off a control must NOT activate it; a scroll page must move
   `scroll.y` when dragged over labels/rows/gaps and a pinned page must stay at
   0 with `flags.scrollable` false; a toggle must persist across re-navigate.
4. **Verified artifact.** Before any screenshot conclusion, confirm the binary
   actually rebuilt: `touch` the edited source and check ninja recompiles it (or
   compare the executable hash/mtime). Re-screenshot only from the fresh build.
   Measure geometry from the tree dump, never from a PNG by eye.

## Geometry Invariants To Enforce (what review_pages.py checks)

- **Single line stays single line.** A label intended for one line (row value,
  summary, chip caption, section title) must have `coords.h <= line_height*1.5`.
  If it can wrap, shorten the copy or widen the container; never accept a wrap
  or an orphaned trailing punctuation mark.
- **No unreachable control.** A visible interactive node (button/switch/slider/
  roller, or `flags.clickable`) with `coords.y + h > viewport_h` (or x overflow)
  inside a page whose content is NOT a scroll owner is clipped and dead. Either
  make the page a real scroll list or fit it. Controls that only sit below the
  fold because a list grew above them are a UX defect: move primary controls up.
- **Disabled is intentional.** `state.disabled` greys a control. Destructive or
  optional rows must NOT be greyed when the action is merely "nothing to do";
  keep red text on the normal surface and give a neutral hint on tap. Grey only
  what is physically unavailable (e.g. pairing while Bluetooth is off).
- **Right icon per state.** In a data state the real image node is visible with
  the expected `image_semantic_id` and the fallback symbol label is hidden; a
  visible `lv_image` with `image_semantic_id == "unknown"` is a broken icon.

## Running The Harness

```sh
cmake --build build/sim
python3 sim/tools/review_pages.py --spec sim/ci/review/spec.json --check
python3 sim/tools/review_pages.py --spec sim/ci/review/spec.json --update   # re-baseline after an approved change
```

It launches its own headless `--ci` sim (fresh NVS), drives each page/state from
the spec, dumps the tree, runs the lint above, and compares a masked PNG to the
baseline in `sim/ci/golden/ui/`. Non-zero exit = a violation or a baseline diff.
Add a page/state to `sim/ci/review/spec.json` whenever you add or restyle one;
states the sim cannot seed (real association, live RF) are marked `hardware_only`
and move to the on-device checklist instead of being silently skipped.

## Baselines Are For Layout And Color, Not Text

Volatile regions (clock seconds, battery %, scan rows, countdown text) are
listed as `masks` in the spec so the pixel gate ignores them; text and geometry
correctness come from the lint, not the image. Re-run `--update` only after you
have looked at the new screenshot and judged it correct.

## Gate Order Before "Done"

`review_pages.py --check` clean -> read every state screenshot against the
invariants above -> gesture cases pass -> binary was the one you changed. Only
then is a UI change reviewed. This tightens the `AGENTS.md` default only for
changes that touch the interface; it stays host/sim-only and adds no hardware or
full-CI burden.

## Simulator Gotchas (cost real debugging time)

- `sim.step` accepts only multiples of 33 ms; a rejected call advances no frames
  and looks exactly like a hung UI. Assert every RPC `ok`.
- After a long navigation session, an intra-app `sim.navigate` to a sibling page
  can be silently dropped (you land back on the previous page). Reach a page by
  hopping through the app root first: `navigate(app,"root")` then
  `navigate(app,page)`. The harness does this and retries.
- Pages with a live clock or looping animation never satisfy `wait_idle`; cap
  the wait and read the tree anyway. `no_golden` such pages (home, clock root,
  setup, diagnostics) so the pixel gate ignores their ticking regions.
- A control clipped below a NON-scrolling page reports `flags.visible=false`, so
  "is it visible" cannot detect it. The tree also exposes `flags.hidden`; treat
  "not hidden but `coords.y+h > viewport`" as an unreachable control.
- System-layer overlays (task switcher on `lv_display_get_layer_sys`, edge-back,
  global menus) are NOT in `sim.tree` — the dump walks `lv_screen_active()`
  only. Review them from `sim.screenshot` (which composites every layer), and
  drive them through a dedicated agent hook (e.g. `sim.switcher`) because their
  real trigger (HOME double-press) is not reproducible under CI stepping.
- Never delete an object inside its own CLICKED callback (a card/close button
  that hides or destroys the list frees the event target and use-after-frees on
  the next indev step). Defer such work with `lv_async_call`. Likewise keep
  persistent chrome (empty-state label) out of any container you `lv_obj_clean`.
- Confirm the binary rebuilt before trusting a screenshot: `touch` the source
  and check ninja recompiles it, or compare the executable mtime/hash. A no-op
  ninja build reviews a stale UI and hides the very defect you are checking.


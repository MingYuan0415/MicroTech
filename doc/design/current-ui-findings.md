# Current UI Findings

This file records observations about the current implementation. The current
UI is a **review subject**, not a design baseline: its dimensions, colours and
component sizes must not be copied into profiles or targets.

The current target declares a 6.0 mm touch requirement. These known current-UI
touch violations are intentionally left as UI debt in the skills-only change:

- `app_ui_page_set_action()` creates a 42 × 42 px page-header action
  (`layers/apps/common/src/app_ui.c`), which is 4.85 mm at 220 DPI.
- Calendar day cells use `CALENDAR_CELL_HEIGHT = 34` px
  (`layers/apps/calendar_app/src/calendar_app.c`), which is 3.93 mm at 220 DPI;
  the current month exposes 30 clickable day cells in a typical view.
- The chip row's inner buttons are 44 px high, and the display and sound page
  sliders use a 16 px track plus two 14 px extended click pads to form a 44 px
  hit area; all are below the 6.0 mm floor.

`style_audit --tree ... --check` reports these as `TOUCH_TARGET_SMALL` when the
resolved profile/target requires the 6.0 mm floor. They are not suppressed by
the audit and must be fixed in a separate production-UI change. The default
`--check` also reports computed optical padding such as 10/14 px as
`OFF_SCALE_SPACING`, and minor radius findings from the current UI; they are
review input, not calibration data.

The current UI is not evidence that a profile threshold, palette value or
component size is correct. Use `design-contract select --mode current` only to
identify the subject under review; use `select --mode design` for new work.

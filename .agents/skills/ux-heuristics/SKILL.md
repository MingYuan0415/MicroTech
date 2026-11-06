---
name: ux-heuristics
description: Evaluate MicroTech task flows, labels, errors, navigation and cognitive load with Nielsen and Krug usability heuristics. Use for UX review, usability audits, empty/error/loading-state design, and product-level decisions that are not primarily visual styling.
---

# MicroTech UX Heuristics

Use this skill when the question is “can a person complete the task?” rather
than “does it look right?”. It is style-independent: the same flow gets the
same usability verdict in every profile.

## Sources of Truth

- Read the page/flow contract and the target profile's input capabilities.
- Use `visual-design` for visual hierarchy and `aesthetic-review` for visual
  execution; this skill owns task clarity and recoverability.
- Use `ui-review` for the simulator evidence of a flow.
- `references/inspiration.md` records provenance and scope.

## Workflow

1. **Define the task.** Who is doing what, how often, with which input, and
   what counts as success. If the brief does not say, state your assumption.
2. **Walk the flow.** Entry point, primary action, confirmation, error,
   cancellation, return path. Include the state the person expects to survive.
3. **Score the ten heuristics.** Use `references/nielsen.md`; state the current
   score, the worst issues and the fixes. Mark heuristics that genuinely do not
   apply as `n/a` rather than forcing a score.
4. **Apply Krug's tests.** Use `references/krug.md`: self-evidence, scanning,
   the trunk test, and the “half the words” rule.
5. **Check target input.** Touch target, keypad/encoder path, one-hand reach,
   and whether a gesture-only action has a visible alternative.
6. **Rank by severity.** Use `references/severity.md`. A blocker is a task that
   cannot be completed or recovered; a major issue costs confidence or time.
7. **Hand off.** Separate product-flow fixes from visual fixes. Do not prescribe
   a style.

## Required Review Areas

- **Visibility of status** — does the page say what is happening?
- **Match to the user's language** — labels use product/user terms, not system
  internals.
- **User control** — back, cancel, undo, retry, dismissal.
- **Consistency** — same term, same action, same location.
- **Error prevention** — constraints, defaults, confirmation for destructive
  actions.
- **Recognition over recall** — visible options, not memorised codes.
- **Flexibility** — shortcuts for experts, simple path for novices.
- **Aesthetic and minimalist design** — only relevant information; one primary
  action.
- **Transient feedback** — warnings, rejected actions, and short receipts must
  not hide or show an in-flow label: changing a flex/grid child moves every
  control below it. Use `app_ui_toast_show(page->root, text)` for a non-modal,
  auto-dismissing overlay; keep inline labels for persistent state and
  instructions.
- **Error recovery** — what happened, what was preserved, what to do next.
- **Help/documentation** — contextual, task-focused, searchable where relevant.

## Prohibited

- Do not turn a visual preference into a usability finding.
- Do not assume hover, keyboard or a large screen when the target does not have
  them.
- Do not add a modal as the first answer to a flow problem.
- Do not demand more features when removing a step would solve the task.
- Do not claim a flow works from source reading alone; use simulator evidence
  or mark it `UNVERIFIED`.

## Handoff

Report the task, the input mode, the heuristic scores, the severity-ranked
findings, and which findings are product-flow vs visual vs implementation.

# Nielsen's Ten Heuristics

Score each applicable heuristic 0–4. A score of 4 means genuinely excellent,
not “no complaint”. State the current score, the worst issues and the fixes.
The total is the sum of applicable scores normalised to 40:
`sum(scores) / (4 * applicable_count) * 40`.

| # | Heuristic | What to check |
| --- | --- | --- |
| 1 | Visibility of system status | Does every action get timely feedback? Loading, saving, connected, failed? |
| 2 | Match between system and real world | Do labels use the user's language and order? No internal jargon. |
| 3 | User control and freedom | Can the user cancel, go back, undo or dismiss? Is back predictable? |
| 4 | Consistency and standards | Same term, same action and same location across pages. |
| 5 | Error prevention | Constraints, defaults, confirmation for destructive actions, no accidental taps. |
| 6 | Recognition rather than recall | Are options visible? No memorised codes or hidden modes. |
| 7 | Flexibility and efficiency | Is there a fast path for frequent users without making the first path harder? |
| 8 | Aesthetic and minimalist design | Only relevant information; one primary action; no decorative noise. |
| 9 | Error recovery | What happened, what was preserved, what to do next. |
| 10 | Help and documentation | Contextual, task-focused, searchable where relevant. |

## Applicability

- On simple utility screens, heuristics 7 and 10 may be `n/a`; report
  `score / (4 * applicable_count)` and the normalised `/40` value, and state
  the number of `n/a` heuristics rather than printing a false raw `/40`.
- On a keypad-only target, replace hover/focus assumptions with keypad focus
  and visible selection.
- On a touch target, test tap/drag/back and one-handed reach.

## Scoring Bands

| Score (/40) | Meaning |
| --- | --- |
| 36–40 | No severity-3+ issue; at most one minor diagnostic failure |
| 24–35 | Some major issues or several failed diagnostics |
| 12–23 | A catastrophic issue or many failures |
| ≤11 | Core tasks blocked |

## Report Format

| # | Heuristic | Score | Key Issue |
| --- | --- | --- | --- |
| 1 | Visibility of System Status | ? | ... |
| ... | ... | ... | ... |
| **Total** | | **??/40** | |

Follow with 2–3 strengths, 3–5 priority issues, persona red flags, and the
questions that remain unanswered.

# Severity

Use one scale across all UX findings.

| Severity | Meaning | Priority |
| --- | --- | --- |
| 0 | Not a problem / preference | Ignore |
| 1 | Cosmetic | Fix if time |
| 2 | Minor delay or frustration | Schedule |
| 3 | Major task failure | Fix soon |
| 4 | Catastrophic; task cannot complete | Fix immediately |

Weigh:

- **frequency** — how often the path is used;
- **impact** — what happens when it fails;
- **persistence** — one-time or every time.

## Promotion Rules

Promote to blocker/4 when:

- a destructive action lacks confirmation or undo;
- a required value cannot be read or changed;
- back/cancel traps the user;
- a failure loses submitted data silently;
- a control is unreachable on the target input;
- colour is the only signal for a required decision.

Do not promote a visual preference to a usability blocker.

## Report Shape

```json
{
  "task": "...",
  "target": "...",
  "heuristics": [{"id": 1, "score": 3, "issue": "..."}],
  "findings": [
    {"severity": 3, "area": "error-recovery", "node": "...", "message": "...", "fix": "..."}
  ],
  "limitations": ["..."]
}
```

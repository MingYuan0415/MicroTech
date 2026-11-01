# Design Skill Evaluation Rubric

Use this rubric when running a case from `cases/` through a design skill. The
goal is to test whether the skill behaves correctly under a profile/target pair
that is not the current project style or panel.

## Score Dimensions

| Dimension | 0 | 1 | 2 |
| --- | --- | --- | --- |
| Profile fidelity | Ignores or contradicts the profile | Mentions the profile but misses an axis | Uses the profile's roles, density, depth and scale consistently |
| Target fidelity | Uses current panel constants or ignores target | Mentions the target but leaks an assumption | Uses the target's geometry, input and budgets without leakage |
| Universal floor | Misses contrast, touch, state or recovery | Partially checks | Checks all applicable universal rules |
| Style-relative judgment | Treats current style as universal | Some style bias | Judges against the selected profile without demanding another style |
| Evidence and limits | Invents findings or claims a pass | Evidence partial | Cites screenshot/tree/plan and states what is unverified |

Pass: **≥8/10** and no critical failure.

## Critical Failures

Any one of these fails the case regardless of score:

- mentions `368`, `448`, `AMOLED`, `PLUME`, `AZURE` or a raw hex value outside
  the profile as a design input; quoting a `target_id` that contains those
  dimensions is not a design input;
- treats `dark_sparse` as the default or only style;
- uses colour as the only semantic signal;
- proposes blur, spring, particles, layout-property animation or an infinite
  loop on a constrained target;
- claims a visual pass without screenshot/tree evidence;
- reviews against a profile that does not match the evidence without reporting
  `PROFILE_MISMATCH`.

## Procedure

1. Resolve the case's profile/target with `design-contract`.
2. Invoke the named skill with the case brief and resolved context.
3. Save the skill's output.
4. Score each dimension independently.
5. Record the result in a table with the case id, score and critical failures.
6. If a case fails, update the skill instruction, not the rubric, unless the
   rubric itself is wrong.

## Suggested Matrix

Use at least one case from each row:

- a light style on a small low-refresh target;
- a monochrome style on a non-touch target;
- a soft style on a standard target;
- the neutral fallback with no explicit pair;
- a profile mismatch;
- a motion-budget rejection;
- a no-evidence review.

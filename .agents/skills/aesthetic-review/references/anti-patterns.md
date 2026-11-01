# Anti-Patterns

Split findings into universal and style-relative. A universal anti-pattern is
wrong in every profile. A style-relative one is wrong only when the active
profile forbids it.

## Universal

- A page made of identical cards for unequal content.
- Three feature cards with icon, title and two filler lines.
- A hero-metric block where a big number is the only idea.
- Decorative eyebrows, section numbers or rules that carry no information.
- A gradient, glow or blur used as the only sign of quality.
- Every surface carrying the same radius, border and shadow.
- Emoji used as structural icons.
- Invented statistics, fake data or fake social proof.
- Centred text for long reading passages.
- Disabled controls that look active, or active controls that look disabled.
- Colour as the only difference between success, warning and danger.
- A modal used where inline disclosure would do.
- Decorative motion on a high-frequency action.

## Style-Relative

These are findings only when the active profile disallows them or when they
break hierarchy:

- shadows in a border-led profile;
- borders in a shadow-led profile;
- gradients in a profile with `allow_gradients: false`;
- large rounded corners in a `square` profile;
- tiny corners in a `pill` profile;
- dense rows in a `spacious` profile;
- sparse rows in a `compact` profile;
- display type in a compact utility profile;
- expressive decoration in a `decoration: none` profile.

## False Positives to Avoid

- Calling a light page “too bright” because the current project is dark.
- Calling a monochrome page “missing accent” when the target is mono.
- Calling a dense page “cluttered” when the profile is compact and the content
  is a decision table.
- Calling a rounded page “unprofessional” when the profile is soft.
- Calling a flat page “unfinished” when the profile uses border depth.
- Treating a deliberate summary ellipsis as clipping when the full value is
  reachable one step away.

## How to Fix

Prefer the smallest change that restores the rule:

1. remove the decoration;
2. fix spacing or hierarchy;
3. reduce copy;
4. change a role, not a raw value;
5. change the composition, not the font size;
6. only then add a new component or asset.

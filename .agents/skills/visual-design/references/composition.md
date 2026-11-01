# Composition

## Hierarchy

Establish four levels, in order:

1. what page this is;
2. what the person came to do;
3. the supporting facts;
4. what they can do next.

If the loudest element is not level 1 or 2, the hierarchy is wrong. If
everything is equally quiet, the page has no hierarchy.

Use one primary action per screen. Secondary actions are visually quieter; a
third competing action means one of them belongs on a detail screen.

## Composition Budget

Start from the content rectangle, not from a widget arrangement:

- title region;
- primary content;
- supporting content;
- action region;
- safe/scroll region.

Decide the one element that should be remembered. Spend contrast, size or
unusual placement there. Keep the rest on a normal rhythm.

Do not fill space with cards. A card is justified only when it groups content
that would otherwise be ambiguous. Whitespace, a divider or a change of
alignment is usually cheaper and clearer.

## Density

Use the style profile's `density` axis:

- `compact`: information-first, short rows, tight but consistent rhythm.
- `comfortable`: standard rows, clear grouping, one primary task.
- `spacious`: fewer facts per screen, stronger focus, more breathing room.

Density is not permission to shrink text below the profile's type scale. If
content does not fit, move a secondary fact to a detail screen or reduce copy.

## Lists, Forms and Status

- A list row is for one decision. A row with two unrelated actions is two rows.
- A form groups fields by the decision they support, not by implementation
  order.
- A status page answers “what is happening now?” and “what can I do?” before
  showing history.
- An empty state says what is missing and what the person can do next.
- An error state says what happened, what was preserved, and how to recover.

## Visual Focus

Use one of these, not all of them:

- a single large numeric or textual fact;
- a strong hero control;
- a data relationship (chart, ring, timeline);
- a meaningful image or object;
- a bold typographic treatment.

A page with no focus reads as a list of components. A page with three foci
reads as a template.

## Layout Adaptation

Composition is structural. Use Flex for ordered rows and Grid for aligned
two-dimensional data. Do not fake a different style with fixed coordinates.
When the target is narrow or short, change the composition (stack, collapse,
split) rather than shrinking everything.

`lvgl-ui-layout` owns the exact sizing. This file only decides the intended
relationships.

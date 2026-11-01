# Aesthetic Review Rubric

Every finding names a rule, an evidence location and a fix. Use severity:

- **blocker** — unusable, unreadable or inaccessible; content clipped; a control
  cannot be reached; a required state is absent at a decision point.
- **major** — reads unfinished or contradicts the profile: wrong hierarchy,
  inconsistent spacing scale, competing primary actions, mismatched component
  vocabulary.
- **minor** — polish: optical alignment, a single off-scale gap, copy tightening.

## Composition and Hierarchy

- Is the page's purpose obvious within one glance?
- Is the primary action obvious and singular?
- Does the loudest element match the task's starting point?
- Are related things closer than unrelated things?
- Does the page have one memorable element rather than several competing ones?
- Is there a reading order, or only a stack of equal blocks?
- Is whitespace doing the grouping, or is every group boxed?

## Typography

- Is the type scale closed and consistent with the profile?
- Are there at most two type families and one clear hierarchy?
- Is body text readable at the target's physical size?
- Do line lengths and line heights fit the content?
- Do headings wrap without orphan tails?
- Are numbers and units typeset consistently?
- Are labels, buttons and values in the right hierarchy level?
- Is any required text clipped, dotted or shrunk to fit?

## Colour and Surface

- Does every colour map to a semantic role?
- Is the accent reserved for action, selection and state?
- Does hierarchy come from spacing/typography before colour?
- Are surface steps limited to what the profile declares?
- Do selected, pressed and disabled states remain distinguishable?
- Is colour ever the only signal?
- Does the palette work on the target's actual colour mode?

## States and Feedback

- Are default, pressed, selected, disabled, loading, empty, error and success
  states present where the product can reach them?
- Does every action produce immediate feedback?
- Is the feedback proportional to the action's frequency?
- Is a destructive action confirmed or undoable?
- Does the page preserve the user's input on failure?
- Does a disabled control explain why it is disabled?

## Copy and Information Design

- Does every label name the thing in the user's language?
- Is the primary action named by what it does?
- Do errors say what happened and how to recover?
- Do empty states tell the user what to do next?
- Are units, baselines and dates honest and consistent?
- Is any text present only to decorate?

## Profile Conformance

- Do surfaces match the profile's `surface_model` and `depth_model`?
- Do corners and spacing stay on the profile's scale?
- Does density match the style profile's `density` axis?
- Does decoration match the profile's `decoration` level?
- Does the type scale match the profile's `type_scale`?
- Does the icon family match `icon_style`?
- If the page does not match the selected profile, report the mismatch before
  reporting style preferences.

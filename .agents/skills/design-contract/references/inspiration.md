# Inspiration and Scope

The contract is an original MicroTech design-system layer. It borrows the
general idea of separating semantic roles, scales and target capabilities from
design-token systems; no external skill text or schema is copied.

The important local decisions are:

- A style profile is not a theme implementation; it is a design input.
- A target profile describes capabilities and budgets, not a board identity.
- The fallback is `neutral` + `generic_constrained`, not the current project
  style.
- The current `dark_sparse` profile is validated against the C headers so the
  contract cannot silently drift from the implementation.

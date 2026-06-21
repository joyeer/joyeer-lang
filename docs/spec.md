# The Joyeer Programming Language — Specification

> **Status:** Draft v0.1-spec. This document defines the future language. Some
> features are marked **syntax-only** (parsed and type-checked, but not yet
> semantically enforced). Legacy syntax that exists in the current parser
> (`class`, positional/unlabeled calls, `print(message: x)`) is listed in §14 and will
> be removed.

This specification is split into one file per chapter under
[spec/](spec/). Section numbers (`§N`, `§N.M`) are stable across the split;
cross-references like "see §4.3" point to the correspondingly numbered
chapter file below.

---

## Chapters

| § | Chapter | File |
|---|---------|------|
| 0 | Preamble — philosophy, versioning, status legend, notation | [spec/00-preamble.md](spec/00-preamble.md) |
| 1 | Lexical Structure | [spec/01-lexical.md](spec/01-lexical.md) |
| 2 | Type System | [spec/02-types.md](spec/02-types.md) |
| 3 | Declarations | [spec/03-declarations.md](spec/03-declarations.md) |
| 4 | Memory Model ★ CORE ★ | [spec/04-memory.md](spec/04-memory.md) |
| 5 | Expressions | [spec/05-expressions.md](spec/05-expressions.md) |
| 6 | Statements | [spec/06-statements.md](spec/06-statements.md) |
| 7 | Patterns | [spec/07-patterns.md](spec/07-patterns.md) |
| 8 | Error Handling | [spec/08-error-handling.md](spec/08-error-handling.md) |
| 9 | Contracts 🔬 | [spec/09-contracts.md](spec/09-contracts.md) |
| 10 | Effects 🔬 | [spec/10-effects.md](spec/10-effects.md) |
| 11 | Property-Test & Spec Annotations 🔬 | [spec/11-annotations.md](spec/11-annotations.md) |
| 12 | Modules & Imports | [spec/12-modules.md](spec/12-modules.md) |
| 13 | Naming Conventions | [spec/13-naming.md](spec/13-naming.md) |
| 14 | Deprecated / Removed Legacy + Reserved for Future (§15) | [spec/14-legacy.md](spec/14-legacy.md) |
| 16 | Worked Examples | [spec/16-examples.md](spec/16-examples.md) |
| 17 | Grammar Appendix (EBNF) | [spec/17-grammar.md](spec/17-grammar.md) |

> Reading order for newcomers: §0 → §4 (the memory model is what makes Joyeer
> different) → §3 → the rest as needed. §17 is the consolidated grammar.

---

## Appendix: change log

- **2026-05-30** — Initial draft. Locks A + B language features, includes C
  contracts/effects/property as syntax-only.
- **2026-06-06** — Adopt Swift-style ownership keywords (`borrowing` /
  `consuming` / `initializing`, `mutating` receivers, mandatory `consume`
  call-site marker; Decisions D11–D12). Split the single `spec.md` into
  per-chapter files under [spec/](spec/); this file is now the index.
- **2026-06-20** — Consistency pass. Removed user-defined generics and the
  protocol/trait dependencies (`Comparable` / `Display` / `Iterable` /
  `Deinitializable`): v0.1 keeps only built-in generic containers (§2.6).
  Unified function return-type syntax to `:` (§2.3). Integer overflow now
  **traps in all builds** (§5.2). Moved `Any` to reserved (§1.4, §15). Added
  the `??` coalescing operator (§8.5). Resolved the duplicate-D3 call-site
  contradiction — labels are mandatory (§5.6). Collapsed enum cases to one
  terse form (§3.4). `&` is now required on every `inout` / `initializing`
  lvalue assignment (§5.3). Merged the reserved-keyword lists and fixed the
  prelude (`precondition` / `fatalError`, §12.4).
- **2026-06-21** — Grammar/lexical consistency pass for implementability.
  Added the `indirect` keyword to the reserved list (§1.4) and removed the
  unused `->` token (returns use `:`, §1.6). Defined the `Never` bottom type
  (§2.9) and `param_type` (§2.3); `fatalError` now returns `Never` (§9.2) and
  `Never` is in the prelude (§12.4). Fixed `struct_member` to use `binding`
  (§3.3) and `import_decl` to allow `as` (§3.8). Reconciled the subscript
  `accessor` grammar with §17 so statements may surround `yield` (§3.6).
  Admitted the diverging `return` operand of `??` in the expression grammar
  and fixed the tuple `primary_expr` rule (§17). Noted that `@property`'s
  `forall` quantifier is a reserved annotation DSL, not v0.1 expression
  syntax (§11.2).
- **2026-06-21b** — Deferred string interpolation (`"\(expr)"`) out of v0.1
  to a future version (§1.7, now ⏳); removed `interpolation` from the v0.1
  `string_item` grammar (§1.5) and added it to the reserved syntactic
  constructs (§15).

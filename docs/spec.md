# The Joyeer Programming Language — Specification

> **Status:** Draft v0.1-spec. This document defines the future language. Some
> annotations are marked **syntax-only** (parsed and stored, but not yet
> executed). Removed syntax from earlier implementations or drafts (including
> `class`, `print(message: x)`, and positional/unlabeled calls) is listed in
> §14.

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
| 7 | Match Patterns | [spec/07-patterns.md](spec/07-patterns.md) |
| 8 | Error Handling | [spec/08-error-handling.md](spec/08-error-handling.md) |
| 9 | Contracts 🔬 | [spec/09-contracts.md](spec/09-contracts.md) |
| 10 | General effect system — removed | [spec/10-effects.md](spec/10-effects.md) |
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

- **2026-07-18** — Synchronized §14 with the native-only implementation:
  `class` is reserved and rejected in v0.1; the retired parser/VM compatibility
  behavior is no longer part of the implementation contract.
- **2026-07-13** — Consolidated parser-facing grammar. Made enum construction
  and associated-value labels explicit, added labeled enum pattern payloads,
  and reconciled `return` as the same `Never`-typed expression in standalone,
  `match`-arm, and `??` contexts. Clarified newline statement boundaries and
  bare `return`. Added the implementation-level Parser MVP contract for the
  JSON-parser subset.
- **2026-07-11** — Reduced the lexer and language surface. Removed the general
  effect system (`performs`, `pure`, and effect labels), while retaining the
  ownership access conventions (`borrowing`, `inout`, `consuming`, and
  `initializing`). `match` and payload-carrying enums remain: their patterns
  are parser/type-checker constructs, not a separate lexer subsystem.
- **2026-05-30** — Initial draft. Locks A + B language features, includes C
  contracts/effects/property as syntax-only.
- **2026-06-06** — Adopt Swift-style ownership keywords (`borrowing` /
  `consuming` / `initializing`, `mutating` receivers, mandatory `consume`
  call-site marker; §3.2.4, §4.3). Split the single `spec.md` into
  per-chapter files under [spec/](spec/); this file is now the index.
- **2026-06-20** — Consistency pass. Removed user-defined generics and the
  protocol/trait dependencies (`Comparable` / `Display` / `Iterable` /
  `Deinitializable`): v0.1 keeps only built-in generic containers (§2.6).
  Unified function return-type syntax to `:` (§2.3). Integer overflow now
  **traps in all builds** (§5.2). Moved `Any` to reserved (§1.4, §15). Added
  the `??` coalescing operator (§8.5). Resolved the duplicate call-site
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
- **2026-06-22** — Deferred `break` / `continue` (including labeled forms) and
  the type-test / cast operators (`is`, `as`) to v0.2. They move to the
  reserved list (§1.4, §15); the pattern grammar drops `x as T` (§7, §17), the
  statement grammar drops `break` / `continue` (§6, §17), and `??` / §2.9 no
  longer treat `break` / `continue` as `Never`-typed expressions (§5.12,
  §6.4). The `as` keyword remains for `import` aliasing only (§3.8, §12.3).
- **2026-06-22b** — Editorial consistency pass. Removed the ad-hoc decision
  numbers (`D2` / `D2a` / `D3` / `D7` / `D14`) to comply with §0.4 (decisions
  are cross-referenced by section, not numbered); de-duplicated the
  call-site-label decision (canonical at §3.2.1, referenced from §5.6) and the
  `??` decision (canonical at §8.5, referenced from §5.12). Removed the
  duplicate nil-coalescing row from the §1.6 operator table. Clarified that
  `class` is reserved but accepted-with-warning, not rejected (§15 ↔ §1.4 /
  §14).
- **2026-06-23** — Added §4.10 (Return values & ownership escape): the return
  value is the sole route by which ownership leaves a frame, since reference
  returns do not exist (§4.8); returning a binding is a move at its last use
  (§4.6) that transfers the `deinit` obligation to the caller (§4.7).
  Specified returning through `consuming` (§4.10.1), the `initializing` emplace
  alternative for large results (§4.10.2), the prohibition on returning
  references/projections with subscript `yield` as the in-place substitute
  (§4.10.3), and `Result` / `Optional` / `Never` results (§4.10.4). Linked from
  §3.2.2.
- **2026-06-23b** — Clarified ownership vocabulary. Added §4.1.1 (Ownership
  state vs. access effect): "owned" is a **state** (responsible for `deinit`),
  while `consuming` is the only **effect** that transfers it — `borrowing` /
  `inout` / `initializing` are projections that leave ownership with the
  caller. Decided that an owning binding is mutable in place by its owner and
  that `consuming` parameters / `consuming self` are `var`-like (owned,
  mutable), so a `consuming` method may mutate `self` directly (mutation still
  marked `&`); the `mutating` vs `consuming` distinction is the caller's fate,
  not in-body mutability. Propagated to the §4.2 effect table, §4.2.3, §3.2.4,
  and simplified the §4.10.1 example (removed the rebind-to-`var`).

# The Joyeer Programming Language — Specification

> **Status:** Draft v0.1-spec. This document defines the future language. Some
> features are marked **syntax-only** (parsed and type-checked, but not yet
> semantically enforced). Legacy syntax that exists in the current parser
> (`class`, named-only calls, `print(message: x)`) is listed in §14 and will
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

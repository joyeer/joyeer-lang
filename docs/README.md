# Joyeer Documentation Index

The normative language definition is [spec.md](spec.md). Rationale explains
why decisions were made, implementation notes describe the current compiler,
and plan documents track incomplete work. If they disagree, the specification
wins.

## Start here

| Goal | Document |
|---|---|
| Learn the language | [spec.md](spec.md) |
| Understand the design | [rationale/ai-era-design.md](rationale/ai-era-design.md) |
| Understand memory and ownership | [rationale/memory.md](rationale/memory.md) |
| See the implemented pipeline | [plan/roadmap.md](plan/roadmap.md) |
| See the v0.1 scope | [plan/v0.1.md](plan/v0.1.md) |
| Build from source | [building.md](building.md) |
| Contribute | [AGENTS.md](../AGENTS.md) |

## Specification

The split files under [spec/](spec/) mirror the sections assembled in
[spec.md](spec.md): lexical structure, types, declarations, memory, expressions,
statements, patterns, errors, contracts, modules, naming, examples, and grammar.

## Rationale

| Document | Topic |
|---|---|
| [ai-era-design.md](rationale/ai-era-design.md) | Strong types, ownership, and contracts as AI guardrails |
| [memory.md](rationale/memory.md) | Value semantics, deterministic destruction, no GC/ARC |
| [parameter-passing.md](rationale/parameter-passing.md) | Access conventions and call semantics |
| [runtime-overhead.md](rationale/runtime-overhead.md) | Zero-cost and ABI constraints |

## Implementation

The current compiler has one typed Joyeer IR/LLVM/native pipeline. There is no
bytecode backend or VM.

| Document | Topic |
|---|---|
| [lexer.md](impl/lexer.md) | Token model, spans, recovery, and supported lexical surface |
| [parser.md](impl/parser.md) | Parser grammar, syntax AST, and recovery contract |
| [name-resolution.md](impl/name-resolution.md) | Scopes, symbols, references, and diagnostics |
| [type-checking.md](impl/type-checking.md) | Canonical compile-time types and typed overlays |
| [semantic-analysis.md](impl/semantic-analysis.md) | Return, reachability, initialization, and warning analysis |
| [diagnostics.md](impl/diagnostics.md) | Stable IDs, source rendering, fix-its, and notes |
| [ir.md](impl/ir.md) | Typed backend-neutral IR, verifier, ownership, and lowering |
| [native.md](impl/native.md) | LLVM text emission, Clang linking, native ABI, runtime, and debug artifacts |
| [string.md](impl/string.md) | String representation and operations |

## Plans

Plan documents should be updated or removed when they stop describing active
work.

| Document | Topic |
|---|---|
| [roadmap.md](plan/roadmap.md) | Current pipeline status and next milestones |
| [v0.1.md](plan/v0.1.md) | JSON-parser milestone scope and gaps |
| [session-handoff-2026-07-18-macos.md](plan/session-handoff-2026-07-18-macos.md) | Resume the native-only branch on macOS |
| [feasibility.md](plan/feasibility.md) | Feasibility and combined-risk analysis |

## Conventions

- Keep one topic per document and link instead of duplicating.
- Treat drift between spec and rationale as a bug.
- Keep short-lived plans current; delete obsolete implementation plans.
- Use repository-relative links and verify them when deleting source areas.
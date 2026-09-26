# Joyeer Documentation Index

The normative language definition is [spec.md](spec.md). Rationale explains
why decisions were made, implementation notes describe the current compiler,
and plan documents track incomplete work. If they disagree, the specification
wins.

## Start here

| Goal | Document |
|---|---|
| Learn the language | [spec.md](spec.md) |
| Use Joyeer with an AI coding agent | [Portable Joyeer skill](../skills/README.md) |
| Understand the design | [rationale/ai-era-design.md](rationale/ai-era-design.md) |
| Understand memory and ownership | [rationale/memory.md](rationale/memory.md) |
| See what the compiler implements | [impl/supported-features.md](impl/supported-features.md) |
| See active implementation priorities | [plan/roadmap.md](plan/roadmap.md) |
| Build from source | [building.md](building.md) |
| Install a local Windows Debug compiler | [Debug installation](building.md#local-windows-debug-installation) |
| Contribute | [AGENTS.md](../AGENTS.md) |

## Specification

The files under [spec/](spec/) contain the normative chapters:
lexical structure, types, declarations, memory, expressions, statements,
patterns, errors, contracts, modules, naming, examples, and grammar.
[spec.md](spec.md) is their index and change log, not an assembled copy of
the chapter text.

## Rationale

| Document | Topic |
|---|---|
| [ai-era-design.md](rationale/ai-era-design.md) | AI guardrails, verification boundaries, and feasibility |
| [memory.md](rationale/memory.md) | Value semantics, deterministic destruction, no GC/ARC |
| [parameter-passing.md](rationale/parameter-passing.md) | Access conventions and call semantics |
| [runtime-overhead.md](rationale/runtime-overhead.md) | Zero-cost and ABI constraints |

## Implementation

The current compiler has one typed Joyeer IR/LLVM/native pipeline. There is no
bytecode backend or VM.

| Document | Topic |
|---|---|
| [supported-features.md](impl/supported-features.md) | Current executable language surface, pipeline, and limits |
| [lexer.md](impl/lexer.md) | Token model, spans, recovery, and supported lexical surface |
| [parser.md](impl/parser.md) | Parser grammar, syntax AST, and recovery contract |
| [name-resolution.md](impl/name-resolution.md) | Scopes, symbols, references, and diagnostics |
| [type-checking.md](impl/type-checking.md) | Canonical compile-time types and typed overlays |
| [semantic-analysis.md](impl/semantic-analysis.md) | Return, reachability, initialization, and warning analysis |
| [diagnostics.md](impl/diagnostics.md) | Stable IDs, source rendering, fix-its, and notes |
| [ir.md](impl/ir.md) | Typed backend-neutral IR, verifier, ownership, and lowering |
| [native.md](impl/native.md) | LLVM text emission, backend DLL ABI, native linking, runtime, and debug artifacts |
| [string.md](impl/string.md) | String representation and operations |

## Plans

Plan documents should be updated or removed when they stop describing active
work.

| Document | Topic |
|---|---|
| [roadmap.md](plan/roadmap.md) | Active implementation priorities |
| [joypm-m0.md](plan/joypm-m0.md) | Draft language and host contracts for a project manager written in Joyeer |

## Conventions

- Keep one topic per document and link instead of duplicating.
- Treat drift between spec and rationale as a bug.
- Keep short-lived plans current; delete obsolete implementation plans.
- Use repository-relative links and verify them when deleting source areas.
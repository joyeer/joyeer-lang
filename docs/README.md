# Joyeer Documentation Index

The normative language definition is [spec.md](spec.md), with design
explanations alongside the rules in each chapter. Implementation notes
describe the current compiler, and plan documents track incomplete work.
Explanations and plans do not add language rules or implementation guarantees.

## Start here

| Goal | Document |
|---|---|
| Learn the language | [spec.md](spec.md) |
| Use Joyeer with an AI coding agent | [Portable Joyeer skill](../skills/README.md) |
| Understand the design and cost goals | [spec/00-preamble.md](spec/00-preamble.md) |
| Understand memory and ownership | [spec/04-memory.md](spec/04-memory.md) |
| Understand parameter access | [Access effects](spec/04-memory.md#42-access-effects-on-parameters) |
| Understand contracts and verification limits | [spec/09-contracts.md](spec/09-contracts.md) |
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

Design rationale, design notes, and cost notes explain the adjacent rules.
They are not separate specifications; future proposals remain explicitly
distinguished from adopted rules and current implementation support.

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
| [native.md](impl/native.md) | LLVM emission, backend ABI, linking, runtime, debug artifacts, and performance measurement |
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
- Keep design explanations beside the rules they motivate; do not repeat
  normative rules in separate design documents.
- Keep implementation details and measurements out of normative rules, and
  distinguish proposed features from implemented ones.
- Keep short-lived plans current; delete obsolete implementation plans.
- Use repository-relative links and verify them when deleting source areas.
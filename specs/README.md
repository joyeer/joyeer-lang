# Joyeer Language Specification

> **Draft v0.1-spec.** This describes the language design, not just implemented
> features. Proposals and syntax-only annotations are not implementation
> guarantees; see the [implemented surface](../docs/impl/supported-features.md).

Start with the [preamble](00-preamble.md), [memory model](04-memory.md), and
[declarations](03-declarations.md), then read other chapters as needed.

## Chapters

| § | Chapter |
|---|---------|
| 0 | [Preamble](00-preamble.md) |
| 1 | [Lexical structure](01-lexical.md) |
| 2 | [Type system](02-types.md) |
| 3 | [Declarations](03-declarations.md) |
| 4 | [Memory model](04-memory.md) |
| 5 | [Expressions](05-expressions.md) |
| 6 | [Statements](06-statements.md) |
| 7 | [Match patterns](07-patterns.md) |
| 8 | [Error handling](08-error-handling.md) |
| 9 | [Contracts](09-contracts.md) |
| 11 | [Declaration annotations and proposed test annotations](11-annotations.md) |
| 12 | [Modules and imports](12-modules.md) |
| 13 | [Naming conventions](13-naming.md) |
| 18 | [Portable host operations](18-host.md) |
| 19 | [C interoperation](19-c-interop.md) |
| 20 | [Protocols and bounded generics](20-protocols.md) |
| 21 | [Concurrency](21-concurrency.md) |

Add future chapters at the end of this table, preserving existing chapter
numbers. Appendices always follow all numbered chapters.

## Appendices

| Appendix | Reference |
|---|---|
| A | [Reserved keywords and syntax](appendix-a-reserved.md) |
| B | [Worked examples](appendix-b-examples.md) |
| C | [Grammar (EBNF)](appendix-c-grammar.md) |

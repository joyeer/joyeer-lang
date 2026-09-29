# Implemented Language Surface

> **Status:** current implementation boundary. The
> [language specification](../spec.md) is normative for the broader design,
> but it includes syntax and semantics that the compiler does not yet
> implement. This document records implementation coverage; it is not a
> release-readiness or complete-correctness claim.

## Compilation boundary

The compiler accepts one Joyeer source file and runs it through lexing, parsing,
name resolution, type checking, semantic analysis, verified Joyeer IR, and
textual LLVM IR emission. Native executable output additionally generates
machine code and links it with `JoyeerNativeRuntime`.

## Compiler pipeline

| Stage | Current implementation | Details |
|---|---|---|
| Lexer | Explicit terminals, spans, recovery, and the accepted literal/operator surface | [Lexer](lexer.md) |
| Parser | Syntax-only AST, Pratt precedence, payload enums, `match`, unit values, and recovery | [Parser](parser.md) |
| Name resolution | Stable symbols/scopes and deferred type-directed references | [Name resolution](name-resolution.md) |
| Type checking | Canonical types, calls, access conventions, patterns, and exclusivity | [Type checking](type-checking.md) |
| Semantic analysis | All-paths return, reachability, initialization, and unused-binding analysis | [Semantic analysis](semantic-analysis.md) |
| Joyeer IR | Verified CFG, aggregates, ownership operations, and source/debug scopes | [IR](ir.md) |
| Backend | Textual LLVM IR plus in-process LLVM code generation and LLD linking | [Backend](backend.md) |
| Runtime | Strings, owned byte arrays, collections, typed file input, checked arithmetic, and allocation balance | [Runtime](runtime.md) |
| Diagnostics | Stable stage IDs, source excerpts, fix-its, help, and secondary notes | [Diagnostics](diagnostics.md) |

## Implemented source surface

| Area | Current surface |
|---|---|
| Programs | One source file; typed functions; recursion; `func main()` returning `Void` or `func main(args: [String]): Int` for executables |
| Bindings and values | Function-local `let` and `var`; `Int` (signed 64-bit), `Bool`, `UInt8`, `String`, and unit `()` / `Void` |
| Expressions | Calls with mandatory labels, member access, subscripts, postfix `?` propagation, assignment, checked integer arithmetic, comparisons, Boolean `&&`, and string concatenation |
| Control flow | `if`, `else if`, `else`, `while`, `return`, and exhaustive `match` |
| Aggregates | Concrete structs and payload enums with compiler-managed value copying and destruction |
| Parameters | Default or explicit `borrowing`, `inout`, `consuming`, and `initializing`, with mandatory access markers and exclusivity checks |
| Containers | Compiler-known `Array<T>` / `[T]`, `Dict<K, V>` / `[K: V]`, `Optional<T>` / `T?`, and `Result<T, E>` |
| Dictionary lookup | `Dict.get(key:)` returns an independent owned `Optional<V>`; missing keys return `.None`, without changing strict subscripts |
| Unit results | `Result<Void, E>`, constructed as `.Ok(())`; unit expressions, types, bindings, aggregate fields, container elements, and patterns |
| Strings and bytes | Byte count/indexing, owned `utf8()` byte arrays, the fixed escape set, comparison, concatenation, cloning, and destruction |
| Built-ins | `print(value:)` for supported primitive/string values, `byteToInt(value:)`, `byteToString(value:)`, and `readFile(path:)` |
| Compiler output | Frontend validation, textual LLVM IR, native executables, optimization flags, debug information, and native debug artifacts |

`Array`, `Dict`, `Optional`, and `Result` are compiler/runtime-special-cased
builtins. Their type arguments do not imply support for user-defined generic
declarations.

## Not implemented

- User-defined generics, traits/protocols/interfaces, classes, closures, or
  asynchronous/concurrent functions.
- User-defined `init` / `deinit` bodies, explicit copy initializers, or
  user-defined accessors.
- Top-level executable statements or global storage, modules/imports,
  multi-file compilation, or dependency packages.
- `Float`, `Double`, general tuples, or tuple destructuring.
- `for-in`, `break`, `continue`, `||`, compound assignment, `is` / `as`,
  match guards, range patterns, or alternative patterns.
- Optional chaining `?.`, coalescing `??`, or force unwrap `!`. Optional
  types use `T?`; postfix `?` propagates only between the same `Result` error
  type or between `Optional` results. Other errors require explicit `match`.
- String interpolation, Unicode scalar/grapheme APIs, general formatting,
  aggregate printing, streaming file APIs, or a general standard library.
- Source-language FFI, contracts or property annotations as executable
  language features, incremental compilation, or formal verification.
- Project-manager commands such as `joyeer build`, `run`, `test`, `init`,
  `toolchain`, or `doctor`.

Lexical or specification coverage alone does not make a feature executable.
New source-visible features must pass through every applicable frontend,
Joyeer IR, native backend, runtime, diagnostic, and durable-fixture boundary.

## Semantic and runtime limits

- `String.count` and `String[i]` operate on bytes, not characters. Strings and
  file input can contain arbitrary bytes; valid UTF-8 is not currently
  enforced.
- Ordinary copies of heap-backed values can allocate and recursively clone
  uniquely owned storage. There is no garbage collection, ARC, or copy-on-write
  guarantee.
- Dictionary lookup is linear. `get(key:)` returns an owned `Optional<V>` and
  preserves nested optional layers; copying a stored heap-backed value may
  allocate. Subscript access still traps for a missing key.
- Checked integer arithmetic and bounds checks can terminate a program on
  invalid input in every optimization mode.
- `readFile(path:)` is the implemented file-input primitive and returns
  `Result<String, IOError>`. There is no general filesystem API.
- Generated programs receive Windows command-line arguments as UTF-8 from a
  wide-character entry point, while POSIX arguments preserve their original
  bytes. The entry return status must be in `0..255`; other values produce
  a runtime error rather than truncation. The compiler CLI and existing
  Windows file-path handling still have non-ASCII limitations; portable path
  encoding remains future work.
- Debug emission includes source line tables, lexical variables/types/scopes,
  and PDB/DWARF/dSYM artifact handling. Optimized-value and aggregate-projection
  inspection remains incomplete.
- Diagnostics do not yet provide general multi-line/Unicode-aware rendering or
  machine-readable JSON/SARIF output.

The implementation notes retain stage-specific precision limits and known
correctness gaps:

- [Name-resolution gaps](name-resolution.md#7-known-resolution-gaps)
- [Type-checking gaps](type-checking.md#8-known-correctness-gaps)
- [Semantic-analysis precision limits](semantic-analysis.md#precision-limits)
- [Parser continuation boundaries](parser.md#5-newlines-commas-and-statement-boundaries)
- [IR evaluation, ownership, and verification invariants](ir.md#7-evaluation-and-storage-invariants)

## Keeping this document current

Update this document when implementation support changes. The portable
[Joyeer coding skill](../../skills/joyeer/references/supported-features.md)
must carry the same compatibility boundary while remaining self-contained for
installed distributions. Track unfinished priorities in the
[implementation roadmap](../plan/roadmap.md), not in completed milestone
documents.

# Implemented Language Surface

> **Status:** current implementation boundary. The
> [language specification](../spec.md) is normative for the broader design,
> but it includes syntax and semantics that the compiler does not yet
> implement. This document records implementation coverage; it is not a
> release-readiness or complete-correctness claim.

## Compilation boundary

The compiler accepts one Joyeer source file in legacy mode, or a named root
compilation unit with explicit source-file sets for its dependencies. A module
is a named source set, not a directory: files can span nested or unrelated
directories, and distinct modules can use distinct files from one directory.
It runs the source graph through lexing,
parsing, name resolution, type checking, semantic analysis, verified Joyeer
IR, and LLVM object construction with optional textual IR export. Native executable output additionally
generates machine code and links it with `JoyeerRuntime`.

Named mode uses `--module-name <name>` with positional root files and repeatable
`--module-source <name>=<file>` dependency inputs. Repeated dependency names
add files to one unit; the exact complete name is its logical identity, not an
implicit parent/submodule hierarchy. The compiler never discovers source files,
adds siblings, reads manifests, or fetches dependencies. Build tools choose
source roots and include/exclude rules and pass explicit paths. All supplied
sets are validated before reachable dependencies are compiled, with
canonicalized, sorted paths and duplicate physical-file rejection. The removed
`--module-root` and `--module` flags report migration errors; see the
[CLI/API input contract](backend.md#2-cli).
Explicit inputs must be regular files, but need not use the `.joyeer` extension;
paths containing spaces are supported and require appropriate CLI quoting.

## Compiler pipeline

| Stage | Current implementation | Details |
|---|---|---|
| Lexer | Explicit terminals, spans, recovery, and the accepted literal/operator surface | [Lexer](lexer.md) |
| Parser | Syntax-only AST, Pratt precedence, payload enums, `match`, unit values, and recovery | [Parser](parser.md) |
| Name resolution | Stable symbols/scopes and deferred type-directed references | [Name resolution](name-resolution.md) |
| Type checking | Canonical types, calls, access conventions, patterns, and exclusivity | [Type checking](type-checking.md) |
| Semantic analysis | All-paths return, reachability, initialization, and unused-binding analysis | [Semantic analysis](semantic-analysis.md) |
| Joyeer IR | Verified CFG, aggregates, ownership operations, and source/debug scopes | [IR](ir.md) |
| Backend | LLVM IR objects, textual export, and in-process LLVM code generation and LLD linking | [Backend](backend.md) |
| Runtime | Strings, owned byte arrays, collections, typed file input, checked arithmetic, and allocation balance | [Runtime](runtime.md) |
| Diagnostics | Stable stage IDs, source excerpts, fix-its, help, and secondary notes | [Diagnostics](diagnostics.md) |

## Implemented source surface

| Area | Current surface |
|---|---|
| Programs | Legacy single-file or named explicit compilation units; typed functions; recursion; one root `func main()` returning `Void` or `func main(args: [String]): Int` for executables |
| Modules | Named source-file sets, file-local public short names and qualified imports, use-site ambiguity checks, `private` / `internal` / `public`, dependency-cycle and exposed-type checks, per-file source/debug locations |
| Bindings and values | Function-local `let` and `var`; `Bool`, `Int`/`UInt` (64-bit), signed/unsigned 8/16/32/64-bit integers, `Float`, `Double`, `String`, and unit `()` / `Void` |
| Expressions | Calls with mandatory labels, member access, subscripts, postfix `?` propagation, assignment, signed checked/unsigned wrapping integer arithmetic, floating `+`, `-`, `*`, `/`, comparisons, Boolean `!`, `&&`, `||`, string concatenation, compound assignment (`+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`; not on dictionary elements), and integer bitwise `&`, `\|`, `^`, `~`, `<<`, `>>` |
| Control flow | `if`, `else if`, `else`, `while`, `for x in` over arrays, `String` bytes, and integer ranges (`a..<b`, `a...b`), unlabeled `break` / `continue`, `return`, and exhaustive `match` |
| Aggregates | Concrete structs and payload enums with compiler-managed value copying and destruction |
| Parameters | Default or explicit `borrowing`, `inout`, `consuming`, and `initializing`, with mandatory access markers and exclusivity checks |
| Containers | Compiler-known `Array<T>` / `[T]`, `Dict<K, V>` / `[K: V]`, `Optional<T>` / `T?`, and `Result<T, E>` |
| Dictionary lookup | `Dict.get(key:)` returns an independent owned `Optional<V>`; missing keys return `.None`, without changing strict subscripts |
| Unit results | `Result<Void, E>`, constructed as `.Ok(())`; unit expressions, types, bindings, aggregate fields, container elements, and patterns |
| Strings and bytes | Byte count/indexing, owned `utf8()` byte arrays, the fixed escape set, comparison, concatenation, cloning, and destruction |
| Built-ins | Primitive/string `print(value:)`, fallible byte-exact `writeStderr(contents:)`, byte conversions, legacy `readFile(path:)`, and the portable host functions below |
| Filesystem | UTF-8 paths, binary whole-file `readFileUtf8` and bounded `readFilePrefix`, exclusive `writeFileNew`, directory creation/listing, file classification, file/empty-directory removal, and lexical `joinPath` |
| Processes | Synchronous `runProcess` with explicit executable, argument array and child directory; typed completion and launch/wait errors; no implicit shell or PATH lookup |
| Compiler output | Frontend validation, textual LLVM IR, native executables, optimization flags, debug information, and native debug artifacts |
| Internal C bindings | Explicitly authorized scalar imports/exports, restricted borrowed/initializing buffer imports, scalar or `Void` results, runtime-object output, and explicit native-library link inputs |
| Top-level constants | Top-level `let` with an explicit type and a compile-time initializer: `Bool`, numbers, `String`, and nested arrays of those, up to 1 MiB of data per constant; `public` constants work across modules. Overflow and other evaluation errors are compile-time errors (`ir-lowering.invalid-constant`). Structs, enums, dictionaries and function calls are not supported |
| Optional Compression library | Separately built miniz-backed classic ZIP creation, listing, and bounded in-memory extraction; see [Compression](compression.md) |

`Array`, `Dict`, `Optional`, and `Result` are compiler/runtime-special-cased
builtins. Their type arguments do not imply support for user-defined generic
declarations.

Decimal integer literals use their contextual numeric type, or default to
`Int`; decimal floating literals (including exponents) default to `Double`.
Literal range checking includes the minimum signed values and the full
unsigned 64-bit range. Floating literals are rounded directly to the selected
precision; overflow and underflow to an unrepresentable nonzero value are
diagnosed. Integer literals may also be hexadecimal (`0x`), binary (`0b`), or
octal (`0o`); a plain leading `0` is decimal. A single `_` may separate digits
in any integer literal and in the integer part of a floating literal, but may
not lead, trail, or repeat. Radix literals use the same range checks as decimal
and take the sign from a preceding unary `-`. Values of different numeric types are not implicitly converted;
`Int`/`Int64` and `UInt`/`UInt64` remain distinct source types despite matching
C representations. `x as T` converts between integer types and traps when the
value does not fit; `x &as T` truncates or extends without trapping. Literal
operands take the target type. Floating-point and non-numeric conversions
remain outside this implementation.

Signed integer arithmetic traps on overflow at every optimization level.
Unsigned addition/subtraction/multiplication wrap modulo their width.
Bitwise `&`, `|`, `^` require two integers of the same type; `~` takes one
integer. `<<` and `>>` take an integer on the left and any integer type as the
count; a negative count or one at least the left operand's bit width traps in
every build. `>>` is arithmetic for signed and logical for unsigned types, and
`<<` discards bits shifted out. Integer division/remainder trap on zero; signed minimum divided by `-1`
also traps for either operation. Floating arithmetic uses IEEE 754 behavior,
including signed zero, infinities, and unordered NaN comparisons, without
fast-math assumptions. Floating remainder and unsigned unary minus are not
supported. New scalars can be stored in arrays, fields, and enum/optional/result
payloads; dictionary keys still support only `Int`, `Bool`, `UInt8`, and `String`.

## Source-implemented local project tool

The separate Joyeer-written `joypm 0.1.0` implements `init/check/build/run/test`
under [src/tools/joypm/](../../src/tools/joypm/). Each schema-1 `project.toml`
defines exactly one source/bin/test module; schema-1 `workspace.toml` lists
explicit member directories. Per-module source paths and dependencies,
`--workspace`, and exact `--module` selection are implemented. Legacy project
schemas and `--target` return migration errors. Compiler module semantics
are unchanged; no library ABI is implied.

CMake bootstraps the default `ALL` tool and registers 14 portable gates plus
Windows bootstrap acceptance, independently of `JOYEER_BUILD_UNITTESTS`.
Native ARM64 Debug default build and unfiltered CTest pass 809/809, including
all 15 joypm gates; Debug/optimized self-build and migrated Moss workflows
pass. The prior-layout full Release 789/789 is historical, not fresh acceptance
of the new layout despite the unchanged 0.1.0 version. Every document is
bounded to 65536 bytes with a sentinel
read after regular-file classification. Outputs use module-qualified fresh
generations with 128 claims per module/profile. The tool does not scan
upward, search PATH, delete/cache outputs, fetch external dependencies, or
nested workspaces. These are not streaming or confinement guarantees.
Other native platforms and publication remain open.
See [joypm](joypm.md) for current contracts, parser limits, and validation status.

## Not implemented

- User-defined generics, traits/protocols/interfaces, classes, closures, or
  asynchronous/concurrent functions.
- User-defined `init` / `deinit` bodies, explicit copy initializers, or
  user-defined accessors.
- Top-level executable statements, mutable global storage, import aliases,
  wildcard imports, re-exports, dependency fetching, separate binary modules,
  or a stable source-library ABI.
- General tuples or tuple destructuring.
- `for &x in` (inout iteration), `for` over dictionaries, labeled loop exits, `&&=` / `||=`, `is`,
  match guards, range patterns, or alternative patterns.
- Optional chaining `?.`, coalescing `??`, or force unwrap `!`. Optional
  types use `T?`; postfix `?` propagates only between the same `Result` error
  type or between `Optional` results. Other errors require explicit `match`.
- String interpolation, Unicode scalar/grapheme APIs, general formatting,
  aggregate printing, streaming file APIs, or a general standard library.
- General user FFI, raw native addresses, arbitrary output pointers, C
  aggregates, callbacks, variadic calls, or signatures beyond the specified
  scalar and restricted buffer-import boundary.
  The [C binding implementation](c-interop.md) documents explicit runtime
  authorization, buffer ownership, object output, and native linking.
- Contracts or property annotations as executable language features,
  incremental compilation, or formal verification.
- Project-manager commands on the **compiler**, such as `joyeer build`, `run`,
  `test`, or `init`; use the separate source-implemented `joypm`. Toolchain
  installation, `doctor`, registries, local path packages, and workspaces
  remain deferred.

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
- Legacy `readFile(path:)` returns `Result<String, IOError>` unchanged.
  Portable filesystem operations use a separate `FileSystemError`;
  synchronous processes use `ProcessError` and `ProcessStatus`. See the
  [exact signatures and boundaries](../spec/18-host.md). Recursive cleanup,
  replacement/atomic publication, canonicalization, process output capture,
  timeouts, and asynchronous handles are not provided.
- `writeStderr(contents: String): Result<Void, StderrError>` writes exact bytes
  without appending a newline and flushes; `.WriteFailed(Int)` carries CRT
  `errno`, not a Win32 status. Its regressions pass in native Windows ARM64
  Debug and Release. The bounded
  `readFilePrefix(path: String, maximumBytes: Int)` returns
  `Result<String, FileSystemError>` and is integrated for the 65537-byte
  manifest read after regular-file classification. Its `Int` host descriptor
  maps through name resolution/type checking to a scalar LLVM `i64` input;
  exact boundary/error and allocation-balance tests pass on that target,
  and no streaming API is implied.
- Generated programs receive Windows command-line arguments as UTF-8 from a
  wide-character entry point, while POSIX arguments preserve their original
  bytes. The entry return status must be in `0..255`; other values produce
  a runtime error rather than truncation. The compiler's Windows `wmain`
  UTF-8 conversion and native/UTF-8 linker-path preservation are integrated
  and pass their native Windows ARM64 Debug/Release regressions, including the
  joypm Unicode gate. This is not general Unicode-aware diagnostic rendering
  or evidence for other native targets.
  Existing Windows legacy `readFile` handling remains narrow and unchanged.
  New host operations require strict UTF-8 paths/arguments and use wide
  Windows APIs; invalid bytes are errors, not replacement characters. Host
  operations are not a filesystem sandbox.
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
installed distributions.

# Supported features and compatibility boundary

## Baseline

This skill accompanies the source tree whose CMake project version is `0.0.1`.
The draft specification's version label is not a released compiler version.
The compiler currently has no `--version` option. For a source checkout,
record the revision and build provenance; for a distributed binary, use its
package metadata. A version string alone may not distinguish early-development
builds. If provenance is unknown, say so and validate a minimal program before
relying on these instructions.

Keep this directory with the matching release or revision. The upstream
`docs/spec.md` defines the language design;
`docs/impl/supported-features.md` and native fixtures describe current
implementation coverage. Specification examples can use future features.
Compiler acceptance is evidence of implementation support, not a replacement
for the normative language rules.

The references and examples in this skill are self-contained. They do not
require a source checkout, internet access, or access to a moving branch.

## Implemented surface

| Area | Current surface |
|---|---|
| Programs | Single-file or directory-module compilation; typed functions; recursion; one root `func main()` returning `Void` or `func main(args: [String]): Int` |
| Modules | Explicit directory mappings; file-local qualified imports; `private` / `internal` / `public`; dependency-cycle and exposed-type checking |
| Values | `Int` (signed 64-bit), `Bool`, `UInt8`, `String`, unit `()` / `Void`; local `let` and `var` |
| Control flow | `if`, `else if`, `else`, `while`, unlabeled `break` / `continue`, `return`, Boolean `!`, short-circuit `&&` / `||`, exhaustive `match` |
| Aggregates | Concrete structs and payload enums; compiler-managed value copying and destruction |
| Parameters | Default/explicit `borrowing`, `inout`, `consuming`, `initializing`; access and exclusivity checks |
| Containers | Built-in `Array<T>` / `[T]`, `Dict<K, V>` / `[K: V]`, `Optional<T>` / `T?`, `Result<T, E>` |
| Dictionary lookup | `get(key:)` returns an owned `Optional<V>`; absence is `.None`, while strict subscripts still trap |
| Propagation | Postfix `?` on `Result<T, E>` in `Result<U, E>` functions with identical `E`, or on `Optional<T>` in Optional-returning functions |
| Unit results | `Result<Void, E>` with `.Ok(())`; unit expressions, bindings, fields, container elements, and patterns |
| Strings | Concatenation, comparisons, byte count/indexing, owned `utf8()` byte array |
| Built-ins | Primitive/string `print(value:)`, byte conversions, legacy `readFile(path:)`, portable filesystem functions, synchronous `runProcess` |
| Output | Frontend validation, textual LLVM IR, native executables, optimization and debug flags |

This is an implemented subset with regression coverage, not a claim that every
combination is supported or that the compiler is free of correctness gaps.

## Do not generate these as supported features

- User-defined generics, traits/protocols/interfaces, classes, closures,
  concurrency, or async functions.
- User-defined `init` / `deinit` bodies or explicit copy initializers.
- Top-level executable statements or global storage, import aliases,
  wildcard imports, re-exports, dependency fetching, separate binary modules,
  or a stable source-library ABI.
- Floating-point `Float` / `Double` programs, general tuples, or tuple
  destructuring.
- `for-in`, labeled loop exits, compound assignment such as `+=`,
  `is` / `as` casts, match guards, or range/alternative patterns.
- Optional chaining `?.`, coalescing `??`, or force unwrap `!`. `T?` as an
  Optional type and postfix failure propagation `expr?` are supported;
  use `match` when handling errors at an `Int` entry point.
- String interpolation, Unicode scalar/grapheme APIs, general formatting,
  aggregate printing, streaming file APIs, or an assumed networking library.
- `joyeer build`, `run`, `test`, `init`, `toolchain`, `doctor`, `--target`,
  `--version`, or `--help`. These are not current CLI commands/options.

Use a `while` loop instead of `for-in`,
`x = x + 1` instead of `x += 1`, and
explicit `match` for errors that cannot be propagated with `?`.

## Important semantic and runtime limits

- `String.count` and `String[i]` operate on bytes, not characters. Strings can
  contain arbitrary bytes; file input does not guarantee valid UTF-8.
- Ordinary heap-backed value copies can allocate and recursively clone.
  Do not promise copy-on-write, garbage collection, or allocation-free code.
- Dictionary lookup is currently linear, not hashed. Use `get(key:)` for an
  owned `Optional<V>` that survives dictionary mutation/destruction. Nested
  optional values are not flattened. Missing subscripts still trap.
- Checked integer addition, subtraction, multiplication, division, remainder,
  and array indexing can terminate the program on invalid input. Division and
  remainder trap for zero divisors and minimum `Int` with divisor `-1`.
  Division truncates toward zero; nonzero remainder has the dividend's sign.
  Do not disable checks to fix logic.
- Legacy `readFile(path:)` returns `Result<String, IOError>` unchanged.
  New filesystem APIs return `Result<T, FileSystemError>`; `runProcess`
  returns `Result<ProcessStatus, ProcessError>`. See the language guide for
  exact signatures. Replacement, recursive cleanup, process capture, timeouts,
  and asynchronous handles are not supported.
- Portable host paths and arguments require valid UTF-8 without embedded
  NUL; file contents still preserve arbitrary bytes. The compiler's own
  Windows CLI and legacy `readFile` retain non-ASCII limitations. Report those
  limitations instead of modifying the user's global environment.
- The checked-in JSON parser is an acceptance workload, not a fully conforming
  JSON library or a measured performance guarantee. It omits floating-point
  numbers and Unicode escape decoding, accepts leading-zero numbers and raw
  control characters, and can trap while accumulating the maximum `Int`.

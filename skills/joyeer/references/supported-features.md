# Supported features and version boundary

## Baseline

This skill accompanies the source tree whose CMake project version is `0.0.1`
and whose implementation milestone is called **v0.1**. These are different
labels: the milestone and the draft specification are not a released version.
The compiler currently has no `--version` option. For a source checkout, record
the revision and build provenance; for a distributed binary, use its package
metadata. A version string alone may not distinguish early-development builds.
If provenance is unknown, say so and validate a minimal program before relying
on these instructions.

Keep this directory with the matching release or revision. The upstream
`docs/spec.md` defines the language design; `docs/plan/v0.1.md` and native
fixtures describe current implementation coverage. Specification examples can
use future features. Compiler acceptance is evidence of implementation support,
not a replacement for the normative language rules.

The references and examples in this skill are self-contained. They do not
require a source checkout, internet access, or access to a moving branch.

## Implemented surface

| Area | Current surface |
|---|---|
| Programs | One source file; typed functions; recursion; parameterless `func main()` returning `Void` for executables |
| Values | `Int` (signed 64-bit), `Bool`, `UInt8`, `String`; local `let` and `var` |
| Control flow | `if`, `else if`, `else`, `while`, `return`, short-circuit `&&`, exhaustive `match` |
| Aggregates | Concrete structs and payload enums; compiler-managed value copying and destruction |
| Parameters | Default/explicit `borrowing`, `inout`, `consuming`, `initializing`; access and exclusivity checks |
| Containers | Built-in `Array<T>` / `[T]`, `Dict<K, V>` / `[K: V]`, `Optional<T>` / `T?`, `Result<T, E>` |
| Strings | Concatenation, comparisons, byte count/indexing, owned `utf8()` byte array |
| Built-ins | `print(value:)` for supported primitive/string values; `byteToInt(value:)`, `byteToString(value:)`, `readFile(path:)` |
| Output | Frontend validation, textual LLVM IR, native executables, optimization and debug flags |

This is an implemented subset with regression coverage, not a claim that every
combination is supported or that the compiler is free of correctness gaps.

## Do not generate these as supported features

- User-defined generics, traits/protocols/interfaces, classes, closures,
  concurrency, or async functions.
- User-defined `init` / `deinit` bodies or explicit copy initializers.
- Top-level executable statements or global storage, modules/imports,
  multi-file builds, or dependency packages.
- Floating-point `Float` / `Double` programs.
- `for-in`, `break`, `continue`, `||`, compound assignment such as `+=`,
  `is` / `as` casts, match guards, range/alternative patterns, or tuple
  destructuring.
- Optional chaining `?.`, coalescing `??`, postfix propagation `?`, or force
  unwrap `!`. `T?` as an Optional type is supported; use `match` on the value.
- String interpolation, Unicode scalar/grapheme APIs, general formatting,
  aggregate printing, streaming file APIs, or an assumed networking library.
- `joyeer build`, `run`, `test`, `init`, `toolchain`, `doctor`, `--target`,
  `--version`, or `--help`. These are not current CLI commands/options.

Use a `while` loop with an explicit condition instead of `for-in` or `break`,
`x = x + 1` instead of `x += 1`, nested conditionals instead of `||`, and
explicit `match` for absence and recoverable errors.

## Important semantic and runtime limits

- `String.count` and `String[i]` operate on bytes, not characters. Strings can
  contain arbitrary bytes; file input does not guarantee valid UTF-8.
- Ordinary heap-backed value copies can allocate and recursively clone.
  Do not promise copy-on-write, garbage collection, or allocation-free code.
- Dictionary lookup is currently linear, not hashed. Do not assume a missing
  subscript returns `Optional`; current subscript examples access present keys.
- Checked integer addition, subtraction, multiplication, and array indexing can
  terminate the program on invalid input. Do not disable checks to fix logic.
- `readFile(path:)` is the implemented file-input primitive; it returns
  `Result<String, IOError>`. There is no general-purpose filesystem library.
- Windows command-line and file-path encoding have known non-ASCII limitations.
  Report them rather than treating them as language syntax errors. Do not
  modify the user's global environment as a workaround.
- The checked-in JSON parser is a milestone workload, not a fully conforming
  JSON library or a measured performance guarantee.

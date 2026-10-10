# Supported features and compatibility boundary

## Baseline

This skill accompanies the source tree whose CMake project version is `0.0.1`.
The draft specification's version label is not a released compiler version.
The compiler currently has no `--version` option. For a source checkout,
record the revision and build provenance; for a distributed binary, use its
package metadata. A version string alone may not distinguish early-development
builds. If provenance is unknown, say so and validate a minimal program before
relying on these instructions.

The separate source-implemented `joypm --version` reports `joypm 0.1.0`;
this is an independent early-development tool version, not a compiler version
or release-completion claim. The Windows x64 Debug build with unit tests
enabled and local-project smoke run succeeded. Bounded regular-file manifest
acquisition is integrated and compiled. CTest has not run for this change set;
the nine registered gates, full native acceptance, other platforms, package
installation, and self-build remain pending. Keep those limits with an installed
copy; joypm is not released or release-complete.

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
| Programs | Legacy single-file or named explicit compilation units; typed functions; recursion; one root `func main()` returning `Void` or `func main(args: [String]): Int` |
| Modules | Named source-file sets; file-local qualified imports; `private` / `internal` / `public`; dependency-cycle and exposed-type checking; per-file source/debug locations |
| Values | `Int` (signed 64-bit), `Bool`, `UInt8`, `String`, unit `()` / `Void`; local `let` and `var` |
| Control flow | `if`, `else if`, `else`, `while`, unlabeled `break` / `continue`, `return`, Boolean `!`, short-circuit `&&` / `||`, exhaustive `match` |
| Aggregates | Concrete structs and payload enums; compiler-managed value copying and destruction |
| Parameters | Default/explicit `borrowing`, `inout`, `consuming`, `initializing`; access and exclusivity checks |
| Containers | Built-in `Array<T>` / `[T]`, `Dict<K, V>` / `[K: V]`, `Optional<T>` / `T?`, `Result<T, E>` |
| Dictionary lookup | `get(key:)` returns an owned `Optional<V>`; absence is `.None`, while strict subscripts still trap |
| Propagation | Postfix `?` on `Result<T, E>` in `Result<U, E>` functions with identical `E`, or on `Optional<T>` in Optional-returning functions |
| Unit results | `Result<Void, E>` with `.Ok(())`; unit expressions, bindings, fields, container elements, and patterns |
| Strings | Concatenation, comparisons, byte count/indexing, owned `utf8()` byte array |
| Built-ins | Primitive/string `print(value:)`, fallible byte-exact `writeStderr(contents:)`, byte conversions, legacy `readFile(path:)`, portable filesystem functions, synchronous `runProcess` |
| Output | Frontend validation, textual LLVM IR, native executables, optimization and debug flags |

This is an implemented subset with regression coverage, not a claim that every
combination is supported or that the compiler is free of correctness gaps.

Named mode uses `--module-name` with positional root source files and repeated
`--module-source name=file` dependency inputs. Repeating a dependency name adds
files to one module. The complete name is a logical identity, not an implicit
parent/submodule tree. Directories do not create modules: one source set can
span directories, and distinct sets can share a directory without sharing
files. The compiler never discovers files, adds siblings, reads manifests, or
fetches packages; build tools select files. All supplied sets are validated
before compiling reachable dependencies, with deterministic canonical path
ordering and physical-file duplicate rejection.

Legacy one-file compilation remains supported. Multiple root files or
dependencies require a valid nonempty root name; dependency names cannot equal
the root name. Removed `--module-root` and `--module` flags report migration
errors. See [CLI usage](cli.md#named-compilation-units) for exact input and
path rules. The compiler flags do not add library artifacts, a manifest format,
or cleanup facilities; the separate joypm owns local manifest policy.

## Local project-manager source boundary

`joypm` has `init/check/build/run/test`, strict schema/Unicode/flag validation,
explicit compiler and source paths, debug/release profiles, serial execution,
and always-rebuilt native outputs. Manifest limits are 65536 bytes, 4096 decoded
bytes/string, 256 modules, 256 targets, and 1024 strings/array. The bounded
loader uses `fileKind` to accept only regular-file manifests, rejecting final
symlinks and other nonregular inputs, then reads a 65537-byte prefix so the
parser can reject inputs above 65536. This is integrated and compiled, not
streaming or race-free confinement. Exact-boundary and host regressions are
added but unexecuted.

Project commands require `--compiler <path>` and accept `--manifest-path <file>`
(default `joyeer.toml` in the invocation directory), without upward scanning
or PATH/sibling discovery. Outputs use at most 128 exclusive generation claims
per target/profile. There is no deletion, cache, registry, local path package,
workspace, or self-build acceptance yet. Use trusted local projects only;
programs/tests run with user permissions. See [CLI usage](cli.md#local-projects-with-joypm).

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
  `--version`, or `--help`. These are not compiler commands/options; do not
  confuse them with the separate source-implemented joypm CLI.

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
- `writeStderr(contents:)` returns `Result<Void, StderrError>` with
  `.WriteFailed(Int)` carrying CRT `errno`. It writes exact bytes without a
  newline and flushes; failure may be partial. Its implementation/regressions
  compile, but the regressions have not executed. `readFilePrefix(path:maximumBytes:)`
  returns `Result<String, FileSystemError>` and is integrated and compiled;
  its `Int` parameter resolves and lowers to a scalar LLVM input. Full
  boundary/error acceptance remains pending.
- Portable host paths and arguments require valid UTF-8 without embedded
  NUL; file contents still preserve arbitrary bytes. Windows compiler `wmain`
  UTF-8 and native/linker-path fixes are integrated and compiled, with scoped
  Windows x64 joypm smoke evidence, not general Unicode support. Native
  regressions and the joypm Unicode gate have not run. Legacy narrow `readFile`
  is unchanged; general Unicode-aware diagnostics remain incomplete. Check
  binary provenance and report limits rather than changing global environment.
- The checked-in JSON parser is an acceptance workload, not a fully conforming
  JSON library or a measured performance guarantee. It omits floating-point
  numbers and Unicode escape decoding, accepts leading-zero numbers and raw
  control characters, and can trap while accumulating the maximum `Int`.

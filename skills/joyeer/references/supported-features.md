# Supported features and compatibility boundary

## Baseline

This skill accompanies the source tree whose CMake project version is `0.0.1`.
The draft specification's version label is not a released compiler version.
The compiler currently has no `--version` option. For a source checkout,
record the revision and build provenance; for a distributed binary, use its
package metadata. A version string alone may not distinguish early-development
builds. If provenance is unknown, say so and validate a minimal program before
relying on these instructions.

The separate `joypm --version` reports `joypm 0.1.0`, independently of the
compiler version. Its single-module/workspace organization passes native
Windows ARM64 Debug default build and unfiltered CTest (790/790, including
15 tool gates), plus Debug/optimized self-build and relocated workflows.
A fresh full Release CTest, other native platforms, and publication remain
open. Versions and schemas remain at their initial values during unreleased
development; an older binary with the same version may lack these interfaces.

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
| Modules | Named source-file sets; file-local public short names and qualified imports; use-site ambiguity checks; `private` / `internal` / `public`; dependency-cycle and exposed-type checking; per-file source/debug locations |
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
| Internal runtime bindings | Explicitly authorized named units only: `@c_extern`/`@c_export`, borrowing `Int`/C `int64_t` parameters, explicit `Int` or `Void`/`()` results, and object-only output |

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

Public imported types, constructors, and functions support short names after
ordinary `import`. Existing lexical/module/prelude names take precedence;
ambiguous imported short-name uses reject and require module qualification.
Imports remain file-local and do not re-export their dependencies.

Legacy one-file compilation remains supported. Multiple root files or
dependencies require a valid nonempty root name; dependency names cannot equal
the root name. Removed `--module-root` and `--module` flags report migration
errors. See [CLI usage](cli.md#named-compilation-units) for exact input and
path rules. The compiler flags do not add library artifacts, a manifest format,
or cleanup facilities; the separate joypm owns local manifest policy.

The internal C binding exception uses `--runtime-module <name>` per supplied
unit and `--emit-runtime-object <file>` for a runtime object without startup
or executable linking. It is not general application FFI or a stable
binary-module ABI. Imports and names such as `Foundation` do not grant runtime
context. Ordinary modules use checked, unannotated wrappers rather than
declaring or directly calling C bindings. Static/shared provider selection and
loader deployment belong to the native build system, not joypm manifests.
Do not generate C signatures using `Bool`, `UInt8`, other widths, floating
types, addresses, `Optional`, `Result`, or ordinary aggregates as if supported.

## Local project-manager source boundary

`joypm` has `init/check/build/run/test`, strict TOML/Unicode/flag validation,
explicit compiler paths, and always-rebuilt serial native outputs.
Schema-1 `project.toml` defines one source/bin/test module; schema-1
`workspace.toml` explicitly lists 1..256 member directories. Sources and
dependencies belong to each module. Legacy project files and `--target`
report migration errors. There is no independent source-library binary ABI.

Project commands use `--manifest-path <file>` (default current-directory
project.toml) or `--workspace <file>`, with optional exact `--module` selection.
No upward scanning or PATH/sibling discovery occurs. Each document is bounded
to 65536 bytes with a sentinel read after regular-file classification;
strings are limited to 4096 bytes and module arrays to 1024 entries.
Native outputs use 128 exclusive generation claims per module/profile,
under module-qualified directories. Exact bounds, dependencies, collisions,
failures, and self-build pass on ARM64. These are not streaming/confinement
guarantees. External local modules, registries, nested workspaces, cache and
clean remain deferred. Use trusted projects: children run with user permissions.
See [CLI usage](cli.md#local-projects-with-joypm).

## Do not generate these as supported features

- User-defined generics, traits/protocols/interfaces, classes, closures,
  concurrency, or async functions.
- User-defined `init` / `deinit` bodies or explicit copy initializers.
- General user FFI, native pointer/unsafe syntax, output pointers, callbacks,
  C aggregates/varargs, or C binding types beyond the runtime-only `Int`/`Void`
  subset. `@spec`/`@property` shorthand and DSLs are also unsupported.
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

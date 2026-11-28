# Implemented Language Surface

> **Status:** current implementation boundary. The
> [language specification](../../specs/README.md) is normative for the broader design,
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
sets are validated before rable dependencies are compiled, with
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
| Methods and extensions | Non-generic methods in `struct` and `enum` bodies and in `extension Type { ... }` of a struct or enum declared in the same module; `borrowing` (default), `mutating`, and `consuming` receivers; `self` and `Self`; receiver markers `&recv.m()` and `consume recv.m()`. Fields must be written `self.field`. See [methods](methods.md) |
| Parameters | Default or explicit `borrowing`, `inout`, `consuming`, and `initializing`, with mandatory access markers and exclusivity checks |
| `deinit` and noncopyable types | A `struct` may declare one `deinit() { ... }` (no parameters, `self` is a mutable receiver; it cannot be named or called). It runs once when a live value is destroyed, before its fields are destroyed in reverse order, at scope end, on overwrite and when a consumed value ends in its new owner. A struct with `deinit`, and anything that holds one (a field, an enum payload, or an `Array`, `Dict`, `Optional` or `Result` element), is noncopyable: copying it is `type-checking.copy-of-noncopyable`, and moving a field out of a `deinit` type is `semantic-analysis.partial-consume-of-deinit-type`. A `return` of an owning local moves it, and `consume x` moves an owning local or consuming parameter into a binding, an assignment, an initializer, an enum case, `append` or an array or dictionary element. A temporary's `deinit` runs when its enclosing block ends, not at the end of its statement; a branch of an `if` or a `match` arm whose value is discarded is its own block. A noncopyable place whose value is discarded (an expression statement, the tail of a `Void` body, a `while` body or an `if` without `else`, or a branch of a statement-position `if` or `match`) is only read, not copied. A `deinit` cannot assign `self`. A `match` pattern may bind a noncopyable enum payload by moving it out of an owned temporary or a `match consume x` scrutinee, and `Array.removeLast()` takes an element out of an array. Not yet: reading a noncopyable element in place (`Dict.get`, an array subscript), and a trailing expression that moves a local (use `return`). See [decision 0015](../decisions/0015-noncopyable-and-linear-types.md) |
| Containers | Library `Array<T>` / `[T]` in the bundled `lib/Runtime/collections.joyeer`, bound by `array_type`; syntax, literals, constants, ownership and native marshalling follow its role, not its name ([0026](../decisions/0026-library-array.md)). Library `Dict<K, V>` / `[K: V]` in the same prelude, bound by `dict_type`; syntax, literals and lookup follow its role ([0027](../decisions/0027-library-dict.md)); keys are `Int`, `Bool`, `UInt8` or `String`, checked at the type. `Optional<T>` / `T?` and `Result<T, E>` are ordinary prelude enums declared in `lib/Runtime/roles.joyeer` and bound by role: `T?`, `nil`, wrapping and postfix `?` follow the role, so a user `Optional` never captures them; `==` and `!=` on them follow `Equatable` ([0031](../decisions/0031-equatable.md)) |
| Linear types | A struct declared `linear` (a contextual modifier accepted only in runtime and core sources, `type-checking.linear-reserved` elsewhere) is never destroyed implicitly: a binding that may still hold one, or a type holding one, at the end of its block, a match arm, a function, a `return`, a `?`, a `break` or a `continue` is `semantic-analysis.linear-not-consumed`; overwriting a live one is `linear-overwrite`; dropping a statement value, borrowing an owned temporary, or ignoring a payload of a moved scrutinee with `_` is `linear-result-dropped`. It ends field by field, consuming every field. A linear type has no `deinit`, cannot be a `Dict` key or value, and cannot be iterated with `for`. `JoinHandle<R>` is the one public linear type (see the `spawn` row) ([decision 0015](../decisions/0015-noncopyable-and-linear-types.md)) |
| `Array.removeLast` | `&values.removeLast()` removes the last element and returns it as `Optional<T>` (`.None` when empty); the element moves into the result, so it works for noncopyable elements ([spec §2.6.2](../../specs/02-types.md#262-removing-the-last-array-element)) |
| `Array.consumeEmpty` | `consume values.consumeEmpty()` ends an empty array and frees its storage; it traps (`array not empty`) otherwise, so it is the way to end an array of linear elements after draining it with `removeLast()`. The receiver must be a variable, parameter or field ([spec §2.6.4](../../specs/02-types.md#264-ending-an-empty-array-consumeempty)). `consume` now also starts an expression statement and a `return` value |
| Function values | `(E T, ...): R` function types (spec §2.3.1) with unlabeled parameters and `borrowing`, `inout`, `consuming` or `initializing` effects; a top-level function named where it is not called is a value of its type (`let f = square`), called positionally with the same markers as a call by name (`f(x, &y, consume z)`); a field, element or call result is called through a parenthesized callee (`(s.f)(x)`). Values are copyable, never linear, and may be stored anywhere except a dictionary key or a top-level constant, and may cross tasks and channels. Not values: methods, builtins, `@c_extern`/`@c_data`, the root `main`. No closures, no `==`, no `print`; force-unwrap is not implemented, so an optional function is unwrapped with `match` ([spec §5.6.1](../../specs/05-expressions.md), [decision 0020](../decisions/0020-function-values.md)) |
| Protocols and generic functions | Top-level `protocol` declarations, explicit conformance extensions of same-module user structs/enums, and top-level generic functions (`<T: P & Copyable>`), checked once and monomorphized with direct witness calls. Type arguments are inferred from arguments, not the result; they may be noncopyable but never linear or `Never`. A parameter is noncopyable without `Copyable`. Requirement effects use the ordinary access markers and ownership/exclusivity checks. No generic function values or generic `spawn` targets. Generic methods, defaults/refinement of protocols, associated types and existential values remain deferred ([spec §20](../../specs/20-protocols.md), [decision 0021](../decisions/0021-protocols-and-generic-functions.md)) |
| Equality (`Equatable`) | `==` and `!=` type-check when both operands have one type that conforms to the role prelude's `Equatable` ([0031](../decisions/0031-equatable.md), spec §20.13). The integer types, `Bool`, `UInt8`, `Float`, `Double`, `String` and `Void` conform by the compiler and compile as before (`Float`/`Double` keep IEEE equality); a user struct or enum conforms by an `extension X: Equatable` with a hand-written `equals(other:)`, called directly by `==` with both operands borrowed, so a noncopyable conformer compares too; `T: Equatable` gives `==` and `t.equals(other:)` in generic code. A linear type cannot conform, and `Equatable` cannot name a user type or type parameter. An empty `extension X: Equatable {}` derives equality: fields in declaration order, or tags then payloads, stopping at the first difference, in a generated witness per concrete type; a generic type conforms when its members do for the type arguments, and a member that can never conform is `non-equatable-member` at the extension. An operand without a type of its own (`.Red`, `nil`, `[]`, a number) is typed by the other on either side. `X?` and `Result<X, Y>` conform when their payloads do (the role prelude's two empty extensions), and `[X]` when `X` does, through the collections prelude's `arrayEqual<T: Equatable>`; `Dict` never conforms. `x == nil` and `x == .None` test presence on any optional, whatever its payload. Recursive types conform through arrays, since `indirect` is not implemented. A replacement role prelude must declare `Equatable`. See [validation](#equality-validation). |
| Generic structs and enums | `struct Box<T: P> { ... }` and `enum Tree<T> { ... }`, with `extension Box<U>` listing exactly the declaration's parameters and unconditional conformance. Type arguments are inferred from an initializer or case arguments, then from the expected type (binding annotation or similar); `Box<Int>(...)` is not an expression form. A parameter is noncopyable without `Copyable`; a type owning raw memory needs a `deinit`, which runs once per instance. Expanding recursion through fields is rejected. See [spec §20.12](../../specs/20-protocols.md), [decision 0022](../decisions/0022-generic-types-and-layout-intrinsics.md) |
| Layout intrinsics | In authorized runtime units: `joyeer_intrinsic_size_of<T>()`, `align_of<T>()`, `move_in<T>(address:value:)`, `move_out<T>(address:)`, `destroy_at<T>(address:)`, `stack_slot<T>()`, `needs_destroy<T>()`, `callback_address<T...>(function:)`, and the collection intrinsics `clone_at<T>`, `destroy_range<T>`, `clone_callback<T>`, `destroy_callback<T>` ([0025](../decisions/0025-library-collection-capabilities.md)) and `slot_address<T>(base:index:)`, `stack_block<T>(count:)` and `place_address<T>(value:)` ([0026](../decisions/0026-library-array.md)), and `borrow_address<T>(value:)` and `key_kind<K>()` ([0027](../decisions/0027-library-dict.md)). Addresses are `Int`; callback entries have an exact consuming-Int/Void signature. Linear layout transfers are restricted to closed compiler-prelude task declarations ([0023](../decisions/0023-library-concurrency.md)) |
| `spawn` and `JoinHandle<R>` | Ordinary generic source-prelude declarations. `let h = spawn(function: work, input: consume value)` runs the named nongeneric top-level function `work` (one `consuming` parameter); `consume h.join()` waits and returns its result. Handles are linear, including task inputs/results; arrays of them drain with `removeLast()` and end with `consumeEmpty()`. At most 256 unjoined tasks; a task trap ends the process. A program that calls `spawn` cannot call or take a function value of `System.runProcess` or `System.startProcess`. Public generic targets and stored function-value targets remain rejected ([spec §21](../../specs/21-concurrency.md), [0023](../decisions/0023-library-concurrency.md)) |
| Channels | Source-prelude generic `deinit` structs and an ordinary `ChannelClosed` enum. `channel(capacity: 8, sender: &s, receiver: &r)` creates a bounded single-sender/single-receiver channel; `send` and `receive` return `Result`, `waitForChannel` waits for a ready receiver without dequeuing, and consuming `close()` or destruction closes an end. Ends are noncopyable, not linear, and move into tasks. Linear elements and channel creations that could put their own receiver in a message are rejected. Matching `receive()` directly moves bound payloads without a clone ([spec §21.9](../../specs/21-concurrency.md#219-channels)). |
| Dictionary lookup | `Dict.get(key:)` returns an independent owned `Optional<V>`; missing keys return `.None`, without changing strict subscripts |
| Unit results | `Result<Void, E>`, constructed as `.Ok(())`; unit expressions, types, bindings, aggregate fields, container elements, and patterns |
| Strings and bytes | Byte count/indexing, owned `utf8()` byte arrays, the fixed escape set, comparison, concatenation, cloning, and destruction |
| Built-ins | Primitive/string `print(value:)` and byte conversions |
| Contracts | Always-on `assert(condition:message:)`, `precondition(condition:message:)`, and `fatalError(message:): Never` are bundled prelude functions; a failure stops without unwinding ([§9](../../specs/09-contracts.md)) |
| Filesystem (`System`) | UTF-8 paths, binary whole-file `readFileUtf8` and bounded `readFilePrefix`, exclusive `writeFileNew`, same-filesystem `replaceFile`, directory creation/listing, file and executable-candidate classification, POSIX link-target reads, file/empty-directory removal, and lexical `joinPath` |
| Digests (`Foundation`) | One-shot and incremental CRC-32 and SHA-256 over arbitrary bytes. `sha256(bytes:)` returns an optional lowercase hex digest; `sha256Start()`, `sha256Update(state:bytes:)`, and `sha256Digest(state:)` support chunks. Updates past the SHA-256 limit (fewer than 2^64 bits) return `false` without changing the state; reading a digest does not consume it |
| TCP (`System`, provisional) | IPv4 literal addresses, timeouts, and advisory `tcpPoll` for up to 64 connections and listeners. `TcpReadQueue` owns up to 4096 connected sockets and waits by stable index without scanning the set (IOCP on Windows, one-shot `epoll` on Linux, `kqueue` on macOS). Native loopback validation passed on Windows x64 and ARM64, macOS ARM64, and Linux AArch64 under WSL2. See [spec §18.6](../../specs/18-host.md#186-tcp-networking) and [validation](../decisions/0032-tcp-read-queue.md#evidence). |
| Processes (`System`) | Synchronous `runProcess`, or `startProcess` with an owned `ChildProcess` and consuming `waitProcess`. Explicit executable, argument array and child directory; no implicit shell or PATH lookup. Dropping the owner blocks until the child finishes |
| Host queries and clocks (`System`) | Read-only `environmentVariable`, `currentDirectory`, `executablePath`, and the monotonic and wall clocks ([§18.7–8](../../specs/18-host.md#187-process-environment-and-current-directory)) |
| Owned sequential I/O (`System`) | Noncopyable `FileReader`, `FileWriter` and `DirectoryReader`, with deterministic cleanup and explicit consuming close ([§18.9](../../specs/18-host.md#189-owned-sequential-files-and-directories)) |
| Compiler output | Frontend validation, textual LLVM IR, native executables, optimization flags, debug information, and native debug artifacts |
| Internal C bindings | Explicitly authorized scalar imports/exports, restricted borrowed/initializing buffer imports, bare `[UInt8]` data pointers (`pointer:`, borrowed or `inout`), runtime-only memory intrinsics, scalar or `Void` results, runtime-object output, and [native link inputs by file or logical name](c-interop.md#3-static-and-dynamic-providers) |
| System library (Joyeer) | `import System` provides target-platform detection with `isWindows()` (§18.1), explicit-syntax lexical path helpers, §18.2 filesystem operations, legacy `readFile`, `runProcess` (§18.3), `writeStderr` (§18.4), and `readStdinLine` (§18.5). The platform variants use Win32, Linux system calls, or macOS libSystem. Standard input and error use the runtime's streams; macOS file I/O uses descriptors and awaits native acceptance. See [host-operation validation](#host-operation-validation) |
| Platform selection | `@platform(windows, linux, macos, posix)` (one or more names) on top-level functions ([§11.5](../../specs/11-annotations.md#115-platform)). The selected variant is kept and the others are removed before name resolution; the target is the host. `CompileOptions.targetPlatform` can name another, but only for `validate` output (`driver.unsupported-target-platform` otherwise); there is no command-line option. Dropped variants must parse and keep valid annotations but are not resolved or type-checked; a failed selection reports only `platform.*` diagnostics |
| Top-level constants | Top-level `let` with an explicit type and a compile-time initializer: `Bool`, numbers, `String`, and nested arrays of those, up to 1 MiB of data per constant; `public` constants work across modules. Overflow and other evaluation errors are compile-time errors (`ir-lowering.invalid-constant`). Structs, enums, dictionaries and function calls are not supported |
| Optional Compression library | Separately built miniz-backed classic ZIP creation, listing, and bounded in-memory extraction; see [Compression](compression.md) |

`Array`, `Dict`, `Optional` and `Result` are library declarations bound by closed roles; no container is
compiler-defined. See the [collection implementation and validation](runtime.md#collections).
User generic structs/enums are supported independently through §20.12.

`--report-instantiations` prints one line per concrete generic function
instance, sorted by qualified function name and type-argument display names:
`name<Args> | required by caller at file:line | N IR instructions`. Methods are named for the receiver, `Box<Int>.get`; instances of generic structs and enums follow as `Box<Int> | struct | N fields` and `Tree<Double> | enum | N cases`.
The count is before optimization, identical at every `-O` level, and is not
machine-code size. The requiring call is nearest a non-generic root, ties
broken by least source position. Named modules qualify function names;
same-module private-name collisions add `@sourceId`. Type names are qualified
when otherwise ambiguous. Names are printed in full. Without another output
option the flag creates no file. Limits are 10,000 instances, type nesting
and shortest-chain depth 64, and 4,096 expanded type occurrences per argument;
exceeding a limit is `type-checking.instantiation-limit`.

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
value does not fit; `x &as T` truncates or extends without trapping; `x as? T` returns `Result<T, ConversionError>` (`.Err(.OutOfRange)` instead of a trap). Literal
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
payloads; dictionary keys support only `Int`, `Bool`, `UInt8`, and `String`, and any other key type is
rejected at the type (`type-checking.invalid-type-argument`).

### Equality validation

The [platform validator](../building.md#full-platform-validation) passed in
Debug and Release on Windows x64 and ARM64, Linux x86-64 and AArch64, and
macOS ARM64. The Windows ARM64 and WSL2 AArch64 runs passed all 24
`E2EEquatable` fixtures, covering builtin, derived, generic, container, and
ownership cases. See [0031](../decisions/0031-equatable.md#platform-validation)
for the exact revisions and results.

### Host-operation validation

The filesystem, process and standard-stream fixtures, `SystemTests`, and fern
bootstrap passed full Debug and Release validation on Windows x64 and Linux
x86_64 (WSL2) at [`a14b5f2`](runtime.md#collection-validation). On macOS ARM64,
the filesystem retry and prefix-reader changes based on `8289622` passed
CTest 2023/2023 and `fern test` 7/7 in each configuration, without reruns,
using LLVM 22.1.8 and SDK 26.5. The run covered Compression, moss, and the
macOS ABI, exact error-code and interrupted-call checks. Intel and universal
macOS builds are rejected at configure time. Windows and Linux platform
acceptance beyond the recorded runs is tracked in the
[runtime plan](../plan/0008-runtime-in-joyeer.md#phase-3-remaining-platforms).
See [reader allocation costs](runtime.md#reader-allocation-costs) for the
measured prefix and stdin tradeoffs.

### Standard-library validation

On 2026-11-22, the changes for plan 0003 passed native macOS ARM64
[validation](../building.md#full-platform-validation) on the implementation
based on `723e543`, with LLVM 22.1.8 and macOS SDK 26.5. Debug and Release
each passed **2,034/2,034 CTest cases** and **7/7 root `fern test` suites**
without retrying failed tests.

- `E2EContractsO0` and `E2EContractsO2` verified eager argument evaluation,
  failure in both profiles, byte-exact panic messages, and no unwinding.
  The runtime death test covered a closed stderr pipe, and the standalone C
  link tests verified that unused contracts do not add runtime imports.
- `E2ESystemQueriesO0` and `E2ESystemQueriesO2` exercised absent, empty and
  nonempty environment values, working-directory lookup, and both clocks.
  `SystemMacOS.InterruptedO0` and `SystemMacOS.InterruptedO2` also exercised
  pre-epoch rounding, overflow, interrupted writes, and native errors.
- `SystemTests` covered sequential file and directory use, close, child exit
  and scoped destruction. The compiler tests rejected forged file owners,
  use after close, and process-launch function values in task programs, including
  when `System` is the root module.
  [Fern adoption](fern.md#standard-library-adoption-validation) covered
  bootstrap, restaging, self-build and relocation.

The full `System` sources also passed frontend checking for Windows, Linux,
and macOS. This was not a native Windows or Linux run of the new APIs.

## Source-implemented local project tool

The separate Joyeer-written `fern 0.1.0` implements `init/check/build/run/test`
under [tools/fern/](../../tools/fern/). Each schema-1 `project.toml`
defines exactly one source/bin/test module; schema-1 `workspace.toml` lists
explicit member directories. Per-module source paths and dependencies,
including local `path-dependencies`, `--workspace`, and exact `--module`
selection are implemented. Legacy project
schemas and `--target` return migration errors. Compiler module semantics
are unchanged; no library ABI is implied.

CMake bootstraps the default `ALL` tool and registers portable acceptance
gates, including bootstrap regeneration, independently of
`JOYEER_BUILD_UNITTESTS`. Native macOS ARM64 Debug and Release pass all 15,
including self-build and relocated workflows with the shared `Foundation`
and `System` helpers. See [standard-library adoption validation](fern.md#standard-library-adoption-validation),
[full platform validation](../building.md#full-platform-validation) for
acceptance commands and [release staging](../building.md#release-staging)
for packaging limits.
Every document is
bounded to 65536 bytes with a sentinel
read after regular-file classification. Outputs use module-qualified fresh
generations with 128 claims per module/profile. The tool supports explicit
workspace nesting and local dependencies, but does not scan upward, search
PATH, delete/cache outputs, or fetch remote packages. These are not streaming
or confinement guarantees.
Unrecorded native targets and publication remain open.
See [fern](fern.md) for current contracts and parser limits.

## Not implemented

- Generic methods (method-level type parameters), conditional conformance, `where` clauses, traits/interfaces,
  classes, closures, or asynchronous/concurrent functions.
- Networking beyond synchronous IPv4 TCP: UDP, IPv6, host names, an exposed
  nonblocking socket mode, TLS, and dynamic readiness sets. The provisional
  [TCP API](../../specs/18-host.md#186-tcp-networking) includes a read queue
  for a fixed set of owned connections.
- User-defined `init` bodies, `deinit` on an `enum` or extension, explicit copy
  initializers, or user-defined accessors.
- Function parameter default values (`= expression`), including on generic
  functions. This existing parser limitation is separate from defaults on
  stored fields.
- Implicit-`self` field access (write `self.count`, not `count`), extensions of
  builtin types or of a type from another module, generic methods, stored
  properties in extensions, and protocol default method implementations.
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
  aggregate printing, or general streaming and asynchronous I/O.
- General user FFI, raw native addresses, arbitrary output pointers, C
  aggregates, callbacks, variadic calls, or signatures beyond the specified
  scalar and restricted buffer-import boundary.
  The [C binding implementation](c-interop.md) documents explicit runtime
  authorization, buffer ownership, object output, and native linking.
- Declaration-level contract syntax, property annotations as executable
  features, incremental compilation, or formal verification.
- Project-manager commands on the **compiler**, such as `joyeer build`, `run`,
  `test`, or `init`; use the separate source-implemented `fern`. `doctor`,
  registries, and remote packages remain deferred.

Lexical or specification coverage alone does not make a feature executable.
New source-visible features must pass through every applicable frontend,
Joyeer IR, native backend, runtime, diagnostic, and durable-fixture boundary.

### Common authoring pitfalls

- `result` and the other [reserved words](../../specs/appendix-a-reserved.md) cannot be
  identifiers. The current unsupported-syntax diagnostic still mentions
  "JSON-parser MVP"; rename the binding rather than treating it as a module
  or file-format requirement.
- `&` is an access marker, not a general marker for mutable bindings.
  Ordinary local reassignment has no `&`; adding it reports
  `type-checking.invalid-access-marker`. Calls, access parameters, and mutable
  field/subscript projections have their own marker requirements; see
  [ownership and access](../../skills/joyeer/references/language-guide.md#ownership-and-access).
- Integer-to-floating-point conversions such as `Int` to `Double` are not implemented. Do not infer APIs from familiar names.

## Semantic and runtime limits

- `String.count` and `String[i]` operate on bytes, not characters, and a byte is
  read-only: `&s[i] = b` is `type-checking.assignment-to-immutable`. Strings and
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
  process operations use `ProcessError` and `ProcessStatus`. See the
  [exact signatures and boundaries](../../specs/18-host.md). Recursive cleanup,
  crash-durable publication, canonicalization, process output capture,
  timeouts, cancellation, and detach are not provided.
- `writeStderr(contents: String): Result<Void, StderrError>` writes exact bytes
  without appending a newline and flushes; `.WriteFailed(Int)` carries CRT
  `errno`, not a Win32 status. The bounded
  `readFilePrefix(path: String, maximumBytes: Int)` returns
  `Result<String, FileSystemError>` and is what fern uses for the 65537-byte
  manifest read after regular-file classification; no streaming API is implied.
- `readStdinLine(maximumBytes: Int): Result<String, StdinError>` reads one
  line from standard input (LF or CRLF stripped, final unterminated line
  returned). `StdinError` has `EndOfInput(Int)`, `LineTooLong(Int)` and
  `ReadFailed(Int)`; a longer line is consumed through its terminator so the
  next call resynchronizes. The bound is required. Windows switches stdin to
  binary mode, and stdout is flushed before the read blocks. See
  [spec §18.5](../../specs/18-host.md#185-standard-input). No streaming, timeout,
  or partial-line API exists.
- Generated programs receive Windows command-line arguments as UTF-8 from a
  wide-character entry point, while POSIX arguments preserve their original
  bytes. The entry return status must be in `0..255`; other values produce
  a runtime error rather than truncation. The compiler's Windows `wmain`
  UTF-8 conversion and native/UTF-8 linker-path preservation are integrated
  and pass their native Windows ARM64 Debug/Release regressions, including the
  fern Unicode gate. This is not general Unicode-aware diagnostic rendering
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

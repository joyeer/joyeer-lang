# LLVM and Native Backend

> **Status:** The v0.1 JSON-parser language surface emits textual LLVM IR;
> the platform backend library validates it, generates machine code, and links
> native executables with in-process LLVM/LLD on Windows, macOS, and Linux.
> The current implementation covers the native acceptance workload and
> regression cases for conditional evaluation, ownership flow, guarded
> payloads, and loop storage. Verifier, native ABI, and library gaps remain.
> User-defined `deinit` and explicit copy initializers are outside the
> implemented v0.1 surface.

---

## 1. Pipeline

The compiler pipeline is:

```text
source
  -> lexer / parser
  -> name resolution
  -> type checking
  -> control-flow semantic analysis
  -> verified Joyeer IR
  -> textual LLVM IR
  -> platform native backend object generation + link
  -> native executable
```

The LLVM emitter lives in `include/joyeer/backend/llvm.h` and
`lib/backend/llvm.cpp`. It emits text so verified Joyeer IR remains separated
from LLVM's C++ model by an inspectable format.

The native linker lives in `include/joyeer/backend/linker.h` and
`lib/backend/linker.cpp`. It calls the versioned C ABI in
`include/joyeer/backend/native_backend.h` on every platform. The
native-backend library parses and verifies textual IR, runs the selected LLVM
optimization pipeline, emits a COFF, Mach-O, or ELF object, and invokes the
corresponding LLD driver in-process. A mutex serializes LLD because its library
entry point has global process state. LLVM/LLD remain private backend
dependencies; the compiler never launches Clang or an LLD executable for
native output.

The DLL discovers MSVC, the Universal CRT, and Windows SDK library directories
through LLVM's Visual Studio Setup Configuration and registry helpers. It then
links the generated object with the sibling `JoyeerNativeRuntime.lib`. The
release package keeps the DLL, runtime archive, and `joyeer.exe` together.

On macOS, CMake resolves the active SDK path and version with `xcrun`. The
backend passes them to the embedded Mach-O LLD driver together with the host
architecture and `libSystem`. Debug builds run the system `dsymutil` only to
materialize a sibling dSYM after code generation and linking are complete.
More precisely, this occurs whenever `-g` enables line tables, regardless of
the compiler optimization level; ordinary native output has no tool
subprocess. On Linux, the private Clang
Driver library discovers the host ABI inputs and returns an ELF linker command
that embedded LLD executes without spawning a process. Supported build SDK
versions and platform prerequisites are documented in
[Building Joyeer](../building.md).

---

## 2. CLI

Validation only:

```pwsh
joyeer source.joyeer
```

Write LLVM IR:

```pwsh
joyeer --emit-llvm output.ll source.joyeer
joyeer -O0 -gfull --emit-llvm output.debug.ll source.joyeer
```

Build a native executable:

```pwsh
joyeer -o output.exe source.joyeer
```

A native executable requires one parameterless `func main()` returning
`Void`. The emitter adds a stable `joyeer_main` trampoline consumed by the C
runtime entry point. Missing or invalid entry signatures are diagnosed rather
than delegated to the platform linker.

Native executable linking has an explicit optimization policy: `-O2` is the
default, and `-O0`, `-O1`, `-O2`, or `-O3` may override it on the CLI. The
selected level controls the backend library's LLVM optimization and
code-generation pipelines on every platform. `--emit-llvm`
intentionally writes the pre-optimization backend IR so it remains
deterministic and inspectable.

Debug information defaults to off (`-g0`). `-g` and `-gline-tables-only`
enable line tables in the host format; `-gfull` adds lexical scopes, source
variables, and physical type metadata. `-gdwarf` selects DWARF 4, and
`-gcodeview` selects CodeView on Windows. Format selection and enable/disable
options compose in command-line order; for example `-gdwarf -g0 -g` produces
DWARF line tables. Debug and optimization are orthogonal: `-O0` marks the
compile unit unoptimized, while `-O1`…`-O3` carry optimized debug flags without
changing the emitted pre-optimization instructions.

Native artifacts follow the host format. Windows CodeView keeps a sibling PDB
with the executable and embeds a CodeView debug-directory reference. Windows
DWARF is emitted by the same backend DLL and keeps DWARF sections in the PE
executable. ELF keeps DWARF sections in the executable; macOS runs `dsymutil`
after the in-process Mach-O link to create a sibling dSYM bundle. `-O0`
disables reference elimination
and identical-code folding for Windows debug links; optimized levels retain
them. Paths cross the DLL as UTF-8 C strings and LLD receives an argument
array, so user paths are not subject to shell expansion.

UTF-8 at the backend boundary does not imply end-to-end Windows Unicode path
support. Narrow `argv` can lose characters, and native temporary-path
construction currently narrows a `std::filesystem::path` through `.string()`.
A temporary directory containing characters outside the active code page can
therefore crash compilation even with ASCII source and output arguments.

### C ABI lifetimes and failure contract

Options and their input buffers are borrowed for the duration of a call.
Diagnostic callbacks are synchronous; their message storage must be copied
if retained beyond the callback. The version string has static ownership and
must not be freed by a caller. Callbacks must not throw or recursively invoke
linking while the serialized LLD operation is active.

The intended boundary translates failures into status codes and diagnostics,
without exposing C++ exceptions or allocator ownership. The failure-path
gaps below mean it is not yet a complete recovery boundary.

---

## 3. LLVM type and ABI mapping

| Joyeer type | LLVM representation |
|---|---|
| `Int` | `i64` |
| `Bool` | `i1` |
| `UInt8` | `i8` |
| `String` | `{ ptr, i64 }` |
| `Array<T>` | `{ ptr, i64, i64 }` |
| `Dict<K,V>` | `{ ptr, i64, i64 }` |
| user `struct` | named LLVM struct with declaration-order fields |
| enum / `Optional` / `Result` | named `{ i32 tag, [N x i64] payload }` |
| `inout T` / `initializing T` | opaque `ptr` |

Enum payload size is the maximum aligned case payload. Pattern lowering emits
source-ordered tag/literal tests and an unreachable miss, assuming frontend
exhaustiveness. Tag branches precede payload interpretation, including nested
patterns; the type checker accounts for refutable payload coverage.

Internal Joyeer calls pass value parameters/results as their LLVM types;
address parameters use pointers. Runtime C calls follow a separate convention:
LLVM decomposes strings and collection handles into pointers/counts, or uses
out-pointers for aggregate results rather than passing C structs by value.
This avoids target-specific C aggregate calling-convention drift.

---

## 4. Runtime

The C11 runtime is in `include/joyeer/native/runtime.h` and
`lib/native/runtime.c`. It currently supplies:

- checked `Int` add/subtract/multiply;
- scalar and string printing;
- string concatenation, equality, ordering, byte indexing, deep clone, and
  destroy, plus owned UTF-8 byte-array extraction;
- explicit byte-to-integer conversion and owned single-byte string creation;
- array construction, checked indexing, mutable element projection,
  ownership-transferring append/growth, recursive clone, and reverse-order
  element destruction;
- dictionary construction, count, linear lookup for primitive/string keys,
  mutable insert/update growth, recursive key/value clone, and destruction;
- binary file input through `readFile(path:)`;
- panic and bounds traps;
- the process entry trampoline and active-allocation balance check.

The runtime uses libc allocation today. Dictionary lookup uses a
straightforward linear implementation; hashing is not yet implemented.

Collection allocations retain element/layout sizes and clone/destroy
callbacks. In the current 64-bit implementation their private headers occupy
24 bytes for arrays and 72 bytes for dictionaries, before payload and allocator
overhead. These callbacks can introduce indirect calls without source-level
protocol dispatch. Allocation/free also updates a relaxed atomic balance
counter; this is accounting, not reference counting.

Joyeer IR `copy`, `take`, and `destroy` operations lower through generated
per-type LLVM helpers. Helpers recurse through structs and tagged payloads and
delegate strings/collections to runtime callbacks. Scope lowering is responsible for destroying owned storage and temporaries in
reverse order on normal and early-return paths. The C entry point fails the
process if runtime-managed allocation count
is nonzero after `joyeer_main` returns, making leaks in native integration
tests observable.

The v0.1 prelude exposes `byteToInt(value:)` and `byteToString(value:)` rather
than silently coercing `UInt8`. LLVM zero-extends the former to the signed
64-bit `Int` representation. The latter allocates one owned byte through
`joyeer_byte_to_string_abi`, so ordinary temporary and scope cleanup applies.

The built-in mutating method
`&values.append(element: value)` requires an addressable mutable `Array<T>`.
Lowering transfers a type-correct `T` to `joyeer_array_append_owned_abi`; the
runtime grows geometrically and preserves the array's element clone/destroy
callbacks. Borrowed heap-backed elements are cloned before transfer, while
owned temporaries move directly into the array.

Mutable `&dictionary[key] = value` lowers to
`joyeer_dictionary_set_owned_abi`. Missing keys grow storage geometrically and
take both key and value. Existing keys retain their original key storage,
destroy the incoming duplicate key and previous value, and take the replacement
value without changing `count`.

Known construction gap: dictionary literals preserve every supplied pair,
including keys that compare equal at runtime. They can therefore have
`count == 2` while lookup/update reaches only the first equal key, unlike
successive insertion into an initially empty dictionary. Construction needs
a defined, consistent duplicate-key policy and ownership cleanup.

### File input

The v0.1 prelude exposes:

```joyeer
readFile(path: String): Result<String, IOError>
```

`Ok` contains an owned byte-preserving `String`, including embedded NUL bytes;
normal ownership cleanup destroys it. `Err` contains `.NotFound(code)`,
`.PermissionDenied(code)`, `.InvalidPath(code)`, or `.Other(code)`. Categories
are stable across platforms; each payload preserves the nonzero platform C I/O
error code. Callers handle both enum layers with exhaustive `match` because
postfix propagation is outside the v0.1 surface.

Byte preservation does not establish valid UTF-8. The reader grows from a
4096-byte buffer and retains spare capacity on success; the returned string's
`count` is its logical length, not its allocated capacity.

LLVM passes the path as pointer/count and separate owned-string/error-code out
pointers to `joyeer_read_file_abi`. The runtime returns a stable C ABI error
category. LLVM then constructs the concrete `IOError` and `Result` tags, so the
runtime does not depend on frontend case ordering or target aggregate layout.
Embedded NUL bytes in a path produce `InvalidPath`. Windows paths currently
use the active narrow-character CRT encoding; a future Unicode path API belongs
to broader standard-library design.

---

## 5. Known limitations

The native path is an MVP, not the final zero-cost implementation:

- copy/destroy helpers cover compiler-known heap-backed values and recursive
  aggregates; user-defined `deinit`, noncopyable user types, and explicit copy
  initializers are not implemented;
- all four parameter effects are accepted; `consuming` supports owning locals,
  consuming parameters, temporaries, and field/subscript projections, while
  `initializing` supports whole mutable local/forwarded storage. Call-site
  and argument-evaluation exclusivity traverse the supported expression
  surface; [type-checking](type-checking.md#8-known-correctness-gaps) and
  [flow-analysis precision limits](semantic-analysis.md#precision-limits)
  remain documented separately;
- allocation balance covers runtime-managed string/collection allocations,
  not arbitrary future unsafe/native allocations;
- aggregate layout has no niche optimization and uses an `i32` tag plus an
  aligned payload buffer;
- all platforms use LLVM's per-module default optimization pipelines;
  no Joyeer-specific pass pipeline or LTO policy is configured;
- the textual emitter can generate source/function/instruction line tables or
  full lexical-scope/variable/type metadata with DWARF 4 or CodeView module
  flags; compiler-generated cleanup/plumbing is suppressed from line rows;
- aggregate debug metadata currently supplies names and sizes with empty
  member lists, not field/payload debugger structure or optimized-value
  location tracking;
- `print` supports primitive and string values, not arbitrary aggregates;
- file input is synchronous and whole-file only; streaming, writing, and
  metadata are not provided.

Conditional evaluation, guarded payload comparisons, prepared-operand cleanup,
and one-time stack allocation are covered by native regressions at O0 and O2.
See the [IR invariants](ir.md#7-evaluation-and-storage-invariants) and remaining
structural verifier limitations.

These gaps must be addressed in Joyeer IR, LLVM lowering, or the native runtime
without creating a second execution pipeline.

### Native ABI hardening gaps

The following failure paths were identified from source and SDK contracts;
they are separate from the reproduced Unicode-path and duplicate-key issues:

- Object output checks stream errors before closing and returns without
  clearing an observed error. LLVM 22.1.8's `raw_fd_ostream` can then terminate
  through its fatal-error destructor path instead of returning `FILE_ERROR`.
  Close-time failures also need explicit handling.
- Linux ELF argument/Clang Driver preparation occurs outside the exception
  guard used by the later linking call. The whole exported operation needs
  exception translation.
- The current IR parser path expects an accessible trailing NUL even though
  the C options describe a pointer/length pair. The internal `std::string`
  caller satisfies this; arbitrary C buffers need a defensive terminated
  copy inside the backend rather than an undocumented extra-byte requirement.
- Invalid C-ABI optimization values currently fall back to O2 instead of
  producing `INVALID_ARGUMENT`.
- Some unrecoverable LLD paths terminate the process; callbacks and fatal
  failures must not be described as universally recoverable status returns.

The existing C ABI smoke test covers version/platform queries and invalid
zero-initialized options. Successful object emission, malformed IR, output
failures, structure-size boundaries, and callback contracts need dedicated
coverage. Disk exhaustion and Linux exception paths were not exercised by
the Windows review.

---

## 6. Validation

Focused tests:

```pwsh
ctest --test-dir build -L llvm-backend --output-on-failure
ctest --test-dir build -L native-runtime --output-on-failure
ctest --test-dir build -L native --output-on-failure
ctest --test-dir build -L file-io --output-on-failure
ctest --test-dir build -L optimization --output-on-failure
ctest --test-dir build -L debug-info --output-on-failure
```

The tests use Clang as an independent textual-IR oracle and use the Windows
backend DLL to compile and run native Joyeer programs. They verify output and
zero allocation balance, stress nested
string/array/dictionary ownership, exercise runtime traps, and reject an
executable request without `main`. File-input tests cover binary bytes,
missing-file errors, exhaustive source-level handling, and zero allocation
balance on both paths. Debug-info tests validate metadata structure in both
DWARF/CodeView modes and make the configured Clang emit objects containing the
corresponding DWARF `.debug_line` and Windows CodeView `.debug$S` sections.
`NativeBackendAbiTests` is compiled as C and verifies the DLL ABI/version
without exposing C++ types.
Native artifact tests additionally validate no-debug output, Windows PDB source
and line records, embedded Windows/ELF DWARF sections, platform artifact
retention, safe metacharacter paths, and refusal to overwrite output/PDB
directories.

`NativeExecutableJsonParser` is the integrated milestone: Joyeer source reads
an external file, recursively parses nested null/Boolean/integer/string/array/
object values, checks representative results, rejects malformed input, and
returns with no runtime-managed allocations. It intentionally matches the
v0.1 scope: floating-point numbers and JSON `\uXXXX` decoding remain deferred.

Optimization tests verify the default and all accepted CLI levels, then compile
at `-O2` and prove checked integer overflow and array bounds still terminate
with their runtime diagnostics. Panic flushes stderr and uses C11 `_Exit` with
a nonzero status, avoiding platform crash dialogs while remaining
unrecoverable. Existing ownership-heavy native tests run at the default `-O2`
and retain the zero-allocation-balance check.

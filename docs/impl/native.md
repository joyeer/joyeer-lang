# LLVM and Native Backend

> **Status:** The v0.1 JSON-parser language surface emits textual LLVM IR;
> Clang validates it, generates machine code, and links native executables.
> Deterministic cleanup and whole-binding `consuming` transfer work for the
> current heap-backed value surface; the broader source-level ownership
> language remains experimental.

---

## 1. Pipeline

The native lane is independent of the compatibility VM:

```text
source
  -> v0.1 lexer / parser
  -> name resolution
  -> type checking
  -> control-flow semantic analysis
  -> verified Joyeer IR
  -> textual LLVM IR
  -> Clang object generation + link
  -> native executable
```

The LLVM emitter lives in `include/joyeer/backend/llvm.h` and
`lib/backend/llvm.cpp`. It emits text instead of linking LLVM's unstable C++
ABI into the compiler. The configured Clang driver parses and verifies that
text before machine-code generation. On Windows, the official LLVM package is
supported even though it does not ship the LLVM C++ development libraries.

The native linker lives in `include/joyeer/backend/linker.h` and
`lib/backend/linker.cpp`. It invokes the configured Clang executable with the
LLVM module and `JoyeerNativeRuntime` archive.

---

## 2. CLI

Validation only:

```pwsh
joyeer --lang=v0.1 source.joyeer
```

Write LLVM IR:

```pwsh
joyeer --lang=v0.1 --emit-llvm output.ll source.joyeer
```

Build a native executable:

```pwsh
joyeer --lang=v0.1 -o output.exe source.joyeer
```

A native executable requires one parameterless `func main()` returning
`Void`. The emitter adds a stable `joyeer_main` trampoline consumed by the C
runtime entry point. Missing or invalid entry signatures are diagnosed rather
than delegated to the platform linker.

The default mode remains the legacy VM for compatibility. Native output
options require `--lang=v0.1`.

Native executable linking has an explicit optimization policy: `-O2` is the
default, and `-O0`, `-O1`, `-O2`, or `-O3` may override it on the CLI. The
selected flag is passed to Clang while it consumes the verified textual LLVM
module. `--emit-llvm` intentionally writes the pre-optimization backend IR so
it remains deterministic and inspectable.

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
| `inout T` | opaque `ptr` |

Enum payload size is the maximum aligned case payload. Pattern lowering emits
source-ordered tag/literal tests and an unreachable miss after the frontend's
exhaustiveness proof.

Runtime calls never pass C structs by value. LLVM decomposes strings and
collection handles into pointers/counts, or uses out-pointers for aggregate
results. This avoids target-specific C aggregate calling-convention drift.

---

## 4. Runtime

The C11 runtime is in `include/joyeer/native/runtime.h` and
`lib/native/runtime.c`. It currently supplies:

- checked `Int` add/subtract/multiply;
- scalar and string printing;
- string concatenation, equality, ordering, byte indexing, deep clone, and
  destroy;
- explicit byte-to-integer conversion and owned single-byte string creation;
- array construction, checked indexing, mutable element projection,
  ownership-transferring append/growth, recursive clone, and reverse-order
  element destruction;
- dictionary construction, count, linear lookup for primitive/string keys,
  mutable insert/update growth, recursive key/value clone, and destruction;
- binary file input through `readFile(path:)`;
- panic and bounds traps;
- the process entry trampoline and active-allocation balance check.

The runtime uses libc allocation today. Collection lookup favors correctness
and a small implementation over performance; dictionary hashing is not yet
implemented.

Joyeer IR `copy`, `take`, and `destroy` operations lower through generated
per-type LLVM helpers. Helpers recurse through structs and tagged payloads and
delegate strings/collections to runtime callbacks. Scope lowering destroys
owned storage and temporaries in reverse order on normal and early-return
paths. The C entry point fails the process if runtime-managed allocation count
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

### File input

The v0.1 prelude exposes:

```joyeer
readFile(path: String): Result<String, Int>
```

`Ok` contains an owned byte-preserving `String`, including embedded NUL bytes;
normal ownership cleanup destroys it. `Err` contains a nonzero platform C I/O
error code. Callers handle both cases with exhaustive `match` because postfix
propagation is outside the v0.1 surface.

LLVM passes the path as pointer/count and passes separate result tag and
payload pointers to `joyeer_read_file_abi`. It also supplies the concrete
`Ok`/`Err` tags, so the runtime does not hard-code frontend case ordering or
pass a tagged aggregate by value. Embedded NUL bytes in a path return an error.
Windows paths currently use the active narrow-character CRT encoding; a future
Unicode path API belongs to broader standard-library design.

---

## 5. Known limitations

The native path is an MVP, not the final zero-cost implementation:

- all four parameter effects are accepted; `consuming` supports owning locals,
  consuming parameters, temporaries, and field/subscript projections, while
  `initializing` supports whole mutable local/forwarded storage. Call-site
  and argument-evaluation exclusivity cover the v0.1 non-escaping projection
  surface;
- allocation balance covers runtime-managed string/collection allocations,
  not arbitrary future unsafe/native allocations;
- aggregate layout has no niche optimization and uses an `i32` tag plus an
  aligned payload buffer;
- no Joyeer-specific LLVM pass pipeline or LTO policy is configured beyond the
  explicit Clang optimization level;
- no DWARF/source debug information is emitted;
- `print` supports primitive and string values, not arbitrary aggregates;
- file input is synchronous and whole-file only; streaming, writing, metadata,
  and a typed I/O error enum are not provided;
- the default CLI mode is still the legacy VM.

These gaps must be addressed without adding new dependencies from v0.1 code to
legacy bytecode or VM runtime descriptors.

---

## 6. Validation

Focused tests:

```pwsh
ctest --test-dir build -L llvm-backend --output-on-failure
ctest --test-dir build -L native-runtime --output-on-failure
ctest --test-dir build -L native --output-on-failure
ctest --test-dir build -L file-io --output-on-failure
ctest --test-dir build -L optimization --output-on-failure
```

The tests make Clang compile generated LLVM IR, compile and run native Joyeer
programs, verify output and zero allocation balance, stress nested
string/array/dictionary ownership, exercise runtime traps, and reject an
executable request without `main`. File-input tests cover binary bytes,
missing-file errors, exhaustive source-level handling, and zero allocation
balance on both paths.

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

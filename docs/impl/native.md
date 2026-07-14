# LLVM and Native Backend

> **Status:** The v0.1 JSON-parser language surface emits textual LLVM IR;
> Clang validates it, generates machine code, and links native executables.
> The runtime and ownership model are still experimental.

---

## 1. Pipeline

The native lane is independent of the compatibility VM:

```text
source
  -> v0.1 lexer / parser
  -> name resolution
  -> type checking
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
- string concatenation, equality, ordering, and byte indexing;
- array construction, checked indexing, and mutable element projection;
- dictionary construction and linear lookup for primitive/string keys;
- panic and bounds traps;
- the process entry trampoline.

The runtime uses libc allocation today. Collection lookup favors correctness
and a small implementation over performance; dictionary hashing is not yet
implemented.

---

## 5. Known limitations

The native path is an MVP, not the final zero-cost implementation:

- ownership/lifetime lowering does not yet insert destroy operations, so
  heap-backed temporary values can leak;
- aggregate layout has no niche optimization and uses an `i32` tag plus an
  aligned payload buffer;
- Clang runs at its default optimization level; a deliberate pass/optimization
  pipeline is not configured;
- no DWARF/source debug information is emitted;
- `print` supports primitive and string values, not arbitrary aggregates;
- no file I/O builtin is available, so the full JSON parser cannot yet read a
  file;
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
```

The tests make Clang compile generated LLVM IR, compile and run a native Joyeer
program, verify its output, exercise runtime collections and traps, and reject
an executable request without `main`.

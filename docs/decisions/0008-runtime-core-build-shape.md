# 0008: How the Joyeer runtime core is built and linked

- Status: accepted and implemented (decided and built 2026-11-07). Verified on
  Windows x64 Debug and Release; Linux and macOS have not built it.
- Spec: none. This is a build contract; the language rules it relies on are
  [§19.4.5](../../specs/19-c-interop.md#1945-compiler-intrinsics).

## Context

[The runtime plan](../plan/0008-runtime-in-joyeer.md) moves the runtime from C into
Joyeer one function at a time. Stage 1 needs the first Joyeer objects inside
`JoyeerRuntime` beside the C ones, without a second runtime archive, without
making the compiler wait for the runtime, and without letting a function have two
providers.

## Options considered

1. **One object per source file or module.** Fine-grained archive members, so a
   program links only the functions it uses. Each file needs its own module name,
   its own `--runtime-module` authorization, and an `import` to call another file,
   which would also need authorizing.
2. **One module of several files, one object** (chosen). The files call each other
   as ordinary functions; one authorization; one build rule. A program that needs
   any runtime-core symbol links the whole object.
3. **A second archive for the Joyeer part.** No change to the C target, but every
   link line, the installer and the packaging inventory change, and "one runtime
   archive" stops being true.

## Decision

- The core is the module `runtime.core`, the files listed in
  `lib/Runtime/CMakeLists.txt` (`JOYEER_RUNTIME_CORE_SOURCES`, explicit, no
  discovery), under `lib/Runtime/core/`.
- A CMake custom command runs the **just-built** `joyeer` with
  `--runtime-module runtime.core --emit-runtime-object` and the files after `--`.
  It depends on `joyeer` and `JoyeerBackend`; nothing in the compiler depends on
  `JoyeerRuntime` for building. The object is an `EXTERNAL_OBJECT` source of the
  existing `JoyeerRuntime` static library.
- Flags: `-O0 -gfull` in Debug, `-O2 -g0` otherwise, so a Debug crash inside the
  runtime shows Joyeer frames.
- **One provider per symbol.** When a function moves, its C definition is deleted
  in the same change. A symbol the C still needs internally calls the exported
  function, not a second copy.
- **The object's symbols are gated.** `RuntimeCoreObjectSymbols` runs `llvm-nm`
  (`scripts/cmake/verifyRuntimeCoreObject.cmake`): the object may define exactly
  the approved exports and import only the approved symbols. Both lists are in
  `tests/unittests/runtime/CMakeLists.txt` and change with the function that needs
  them.
- The exported functions use the existing `@c_export` scalar ABI: a pointer
  parameter is an `Int` address. Compression's C unit calls
  `joyeer_host_valid_utf8` through `runtime.h` unchanged.

## Consequences

- **The gate found a real problem on its first run.** In Debug, a `[UInt8]`
  constant passed to a `pointer:` parameter made the compiler emit array
  clone/destroy helpers that import `joyeer_array_clone_abi` and
  `joyeer_array_destroy_abi`. A core unit that uses any managed type depends on
  the runtime's own array code. Core units therefore use only scalars and
  addresses; the compiler diagnostic that enforces this is D3 of the plan.
  Panic messages cannot be passed as a constant array, which is why string byte
  access moved into the backend (below) and why the plan now lists a
  constant-data-address capability.
- **Byte access has no runtime function.** `text[index]` is a checked inline
  load, like arrays. `joyeer_string_byte_at_abi` was removed from the ABI.
- **Whole-object linking.** A program that uses any core symbol links the whole
  object. If the core grows large, split it into several modules (option 1) with
  an explicit import graph.
- **`memcmp` is still the C library's.** Equality and ordering call it through an
  ordinary binding; the bulk-memory intrinsics (plan stage 3.2) replace it.
- **Performance.** At `-O2` on Windows x64 the Joyeer equality and ordering match
  the C baseline within run-to-run noise (1 KiB: 34.2 ns before and after).
- **Not verified:** Linux and macOS builds, ELF archive grouping and Windows
  whole-archive linking of the object beyond the existing test executables. Those
  are part of platform acceptance (plan stage 9).

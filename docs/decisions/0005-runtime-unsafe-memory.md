# 0005: A runtime-only unsafe memory layer

- Status: accepted and implemented (decided and built 2026-11-07; verified on
  Windows x64 Debug). The runtime is not yet rewritten on top of it.
- Spec: [§19.4.5 Compiler intrinsics](../spec/19-c-interop.md#1945-compiler-intrinsics).

## Context

[Decision 0003](0003-pure-joyeer-filesystem.md) showed that OS calls can be made
from Joyeer with bare byte pointers. Writing the rest of the runtime (strings,
arrays, allocation, process spawning, the entry point) in Joyeer needs more than
that:

- POSIX `posix_spawn` takes `argv` as an array of pointers, so the address of a
  byte buffer must become a value that can be stored in another buffer.
- A string or array implementation reads and writes memory it just obtained
  from `malloc`.
- The allocation counter the tests rely on needs an atomic add.

The language deliberately has no pointer type ([§4.8](../spec/04-memory.md#48-no-reference-types)).

## Options considered

1. **Keep a minimal C substrate** (allocation, entry, panic, process). No
   language change; the runtime is never fully Joyeer.
2. **A runtime-only unsafe layer** reachable only from toolchain-authorized
   units, the same gate `@c_extern` uses. Ordinary programs cannot name it.
3. **A general `unsafe` feature** for any program. Largest commitment; the
   memory-model review is out of proportion to what the runtime needs.

## Decision

Option 2, built as reserved `@c_extern` symbols, not new syntax:

- A C import whose symbol starts with `joyeer_intrinsic_` names a compiler
  intrinsic. The backend lowers it to inline LLVM (`ptrtoint`, unaligned `load`
  and `store`, `atomicrmw add`) and declares no C function for it. The type
  checker and the IR verifier require the symbol to be a known one with exactly
  its signature (shared table in
  [intrinsics.h](../../compiler/frontend/intrinsics.h)).
- **An address is an `Int`.** `address_of` takes a `[UInt8]` as a bare data
  pointer ([§19.4.4](../spec/19-c-interop.md#1944-bare-byte-pointers)) and
  returns its address; the language does not track the array's lifetime.
- **Operations:** `address_of`, little-endian loads and stores of 8, 16, 32 and
  64 bits, and a sequentially consistent 64-bit `atomic_add` that returns the
  previous value. The set is the minimum the runtime needs and can grow.
- **Authorization** is that of every native binding: toolchain-authorized units
  only (`--runtime-module`, or `native-bindings = true` for a standard module).
  Ordinary programs get `type-checking.unauthorized-native-binding`.
- **No safety claim.** These operations can read and write anywhere. They add no
  pointer type, dereference syntax, or address arithmetic beyond `Int`.

Why this shape, over new prelude functions: it needed no change to name
resolution, the prelude, type checking of calls, or lowering, and it inherits
authorization, scalar signature checking, wire transport, and symbol-conflict
checks from the native-binding path. Each runtime unit declares the intrinsics
it uses, which also makes its unsafe surface visible at its top.

## Consequences

- Easier: process spawning on POSIX (a table of argument addresses can be built
  in a byte buffer); strings and arrays implemented in Joyeer; a runtime with
  no C source of our own.
- Harder: runtime units carry a larger review burden than ordinary Joyeer; hot
  paths (string comparison, indexing) compile through our own backend and need
  benchmarks against the C versions before each is accepted.
- Little-endian only. The supported targets are; another would need a
  diagnostic, and none exists yet.
- Tests: unit tests for lowering (inline instructions, no C declaration),
  rejection of unknown, mismatched, exported, and unauthorized uses, and
  tampered IR; and a native fixture that stores and loads every width at
  unaligned offsets, does an atomic add, reads and writes a byte buffer through
  its address, and stores an address into another buffer, in both optimization
  profiles with an allocation-balance check.
- Not covered: pointer lifetimes beyond one call, callbacks, threads, and
  alignment of anything but the atomic add.

## Open questions

- Whether `address_of` should be restricted to the argument of a native call, so
  it cannot outlive the buffer.
- Aligned-access forms for ARM64 hot paths, if unaligned loads cost too much.
- How the entry point and panic are expressed before any allocator exists.

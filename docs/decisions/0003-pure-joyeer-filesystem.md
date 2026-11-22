# 0003: Pure-Joyeer filesystem and bare byte pointers

- Status: accepted and implemented. [0004](0004-remove-host-builtins.md)
  removed the built-ins and made `System` the only provider. See
  [host-operation validation](../impl/supported-features.md#host-operation-validation)
  for the Windows x64, Linux x86_64, and macOS ARM64 results.
- Spec: [§19.4.4 Bare byte pointers](../../specs/19-c-interop.md#1944-bare-byte-pointers)

## Context

The original plan proposed writing the §18.2 filesystem operations in Joyeer,
calling the operating system directly. It listed five FFI extensions:
NUL- and UTF-16-terminated string
adapters (E1), mutable buffers (E2), opaque handles (E3), variadic calls (E4),
and captured native errors (E5). Mapping the compiler's native-binding path
showed that most of that is avoidable.

## Options considered

1. **The five extensions as planned.** Four new parameter conventions through
   the type checker, both IR verifiers, the wire format, and backend call
   lowering, each with per-call temporary allocation. Large, and it moves path
   validation and conversion into generated code.
2. **One extension: a bare byte pointer.** A `[UInt8]` parameter named by
   `pointer:` crosses as a single data pointer; `inout` byte buffers are allowed
   only that way. Everything else is ordinary Joyeer over existing machinery:
   - Paths are built in Joyeer as bytes (UTF-8 plus `0`, or UTF-16LE plus a
     `0` unit), so no string adapter is needed and validation stays in Joyeer.
   - `Int` already carries a handle as a 64-bit value; nothing to add.
   - POSIX uses stdio (`fopen` mode `"wbx"` is exclusive creation) instead of
     the variadic `open`, so no variadic call is needed.
   - `errno` is read through one generic scalar, `joyeer_native_errno()`, in
     the runtime; Windows binds `GetLastError` directly.
   - Output structures (`stat`, `dirent`, `WIN32_FIND_DATAW`) are filled into a
     zeroed `[UInt8]` and read back by index at fixed offsets.
   - Strings are built from bytes by binding the runtime's existing
     `joyeer_string_clone_abi` with an `initializing String` output.
3. **Raw addresses and a general unsafe layer.** Most flexible; contradicts
   [§19.10](../../specs/19-c-interop.md#1910-deferred-memory-and-abi-extensions)
   and the project's review-by-humans goal.

## Decision

Option 2.

- `@c_extern(symbol: "...", pointer: "label", ...)` passes the named `[UInt8]`
  parameters as a bare pointer (§19.4.4). Borrowing or `inout`; `inout` only
  with `pointer:`; imports only; repeated declarations must agree.
  The IR wire format moves to version 8.
- The implementation lives in `System` (`filesystem.joyeer` for the portable
  logic; `filesystem_windows.joyeer` and `filesystem_posix.joyeer` for the
  per-platform primitives behind `@platform`), and `System` declares
  `native-bindings = true`.
- Struct layouts use per-platform constants: `statx` on Linux (mode at
  offset 28), `lstat` on macOS ARM64 (mode at offset 4), directory entry
  names at offset 19 (Linux `getdents64`) and 21 (macOS `dirent`), and
  `WIN32_FIND_DATAW` names at offset 44. Intel and universal macOS builds
  are rejected rather than using the ARM64 file bindings.
- The compiler's built-in filesystem names are unchanged for now. The Joyeer
  versions are reached by qualification (`System.readFileUtf8`) and are checked
  against the built-ins by differential tests.

## Consequences

- Easier: a pointer-sized scalar convention that also serves other native
  bindings; filesystem logic that is reviewable and testable in Joyeer; one
  fewer C source to maintain once routing is decided.
- Harder: the compiler cannot check a binding against the real C prototype or
  that the count passed matches the buffer; those remain the binding author's
  burden. Each binding is exercised on its own platform only.
- Allocation: the Joyeer reader copies through a temporary `[UInt8]`.
  [§18.2](../../specs/18-host.md#182-filesystem-operations) bounds the logical
  read and storage growth, not the number of allocations. See
  [reader allocation costs](../impl/runtime.md#reader-allocation-costs).
- Platform bindings: macOS still uses the deprecated `readdir_r`. Linux now
  calls `getdents64` and `statx` directly, so it needs neither libc wrapper.
- Tests: `tests/c-interop/pointers.joyeer` with a C fixture (both profiles),
  unit tests for accepted and rejected declarations and malformed IR,
  `tests/lib/System/filesystem.joyeer` (over a hundred differential
  comparisons), and a service test that type-checks `System` for every target.

## Evidence

The differential test runs each scenario through the built-in operations and the
Joyeer ones in parallel directory trees and requires identical rendered results,
including native error codes: missing and directory reads, prefix limits around
the file size, exclusive create, directory create/list/remove, kind of files,
directories, and the `NUL` device, invalid and Unicode paths, and lexical joins.
It passes through fern on Windows x64 in Debug and Release.

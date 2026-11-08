# 0003: Pure-Joyeer filesystem and bare byte pointers

- Status: accepted for the FFI extension and the `System` implementation
  (implemented; verified on Windows x64 Debug and Release). The routing
  question was settled by [0004](0004-remove-host-builtins.md): the built-in
  names and `filesystem.c` were removed, and `System` is the only provider.
  The POSIX variants have still not run on Linux or macOS.
- Spec: [§19.4.4 Bare byte pointers](../spec/19-c-interop.md#1944-bare-byte-pointers)

## Context

[docs/plan/filesystem.md](../plan/filesystem.md) proposed writing the §18.2
filesystem operations in Joyeer, calling the operating system directly. The
first plan listed five FFI extensions: NUL- and UTF-16-terminated string
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
   [§19.10](../spec/19-c-interop.md#1910-deferred-memory-and-abi-extensions)
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
- Struct layouts use per-OS constants chosen so each is the same on both
  supported architectures: `statx` on Linux (mode at offset 28), `lstat` on
  macOS (mode at offset 4), `dirent` names at offset 19 (Linux) and 21 (macOS),
  `WIN32_FIND_DATAW` names at offset 44.
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
- Allocation: the Joyeer reader copies through a temporary `[UInt8]`, so it does
  not match the C reader's single allocation and prefix-bounded storage. Nothing
  depends on that today. If the contract in §18.2 is to stay exact, add a
  buffer-to-String path that does not copy before routing.
- Unverified: the POSIX variants type-check for Linux and macOS (a service test
  compiles the real library for all three targets) but have not executed.
  `readdir_r` is deprecated yet still exported on glibc and macOS; `statx`
  needs glibc 2.28. Both must be confirmed on real hosts.
- Open before routing: whether to keep the built-in names compiler-known and
  lower them to `System`, or remove them and require `import System`
  ([plan](../plan/filesystem.md) §5); POSIX run evidence; fern and moss
  switching to the Joyeer implementation; removing the C runtime code and its
  ABI tests.
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

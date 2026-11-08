# 0006: runProcess in Joyeer

- Status: accepted and implemented (decided and built 2026-11-07). Verified on
  Windows x64 Debug and Release; the POSIX variant type-checks for Linux and
  macOS and has not run.
- Spec: [§18.3](../spec/18-host.md#183-synchronous-processes), unchanged. The
  Joyeer function keeps every documented behavior.

## Context

[Decision 0004](0004-remove-host-builtins.md) moves the host operations into
`System`. `runProcess` is the largest: it needs structs filled with handles
and addresses, which [decision 0005](0005-runtime-unsafe-memory.md) made
possible, and it is the one operation that spawns a process.

## Decision

`System.runProcess` is written in Joyeer, in `process.joyeer` (validation) plus
`process_windows.joyeer` and `process_posix.joyeer`.

- **Windows** calls `CreateProcessW` with a `STARTUPINFOW` built byte by byte
  (no intrinsic needed), inheritable duplicates of the three standard handles,
  and the Microsoft C runtime's quoting, exposed as `windowsQuote` so it is
  testable on every host. Resolution of the executable and directory with
  `GetFullPathNameW`, the batch-file refusal, the error classification and the
  wait/terminate cleanup follow the C code.
- **POSIX** uses `posix_spawn` with a `posix_spawn_file_actions_addchdir_np`
  file action instead of `fork`: Joyeer code must not run between `fork` and
  `exec`, because it allocates and counts references. `posix_spawn` returns the
  `errno` of a failed `chdir` or `exec` directly, so the C code's error pipe is
  not needed. `argv` is a table of addresses built in a byte buffer with the
  `address_of` and `store_u64` intrinsics.
- **One new runtime accessor**, `joyeer_native_environment()`, returns the
  address of `environ` (`_NSGetEnviron()` on macOS) because a binding cannot
  name a C global. It sits beside `joyeer_native_errno()`.

## Consequences

- **Minimum libc.** `posix_spawn_file_actions_addchdir_np` needs glibc 2.29 or
  macOS 10.15. musl has no equivalent yet; a musl target would need a different
  strategy and a new decision.
- **Descriptors.** The C code closed nothing and neither does this; the child
  inherits every open descriptor, as before.
- **Tests.** `SystemTests` compares the built-in and Joyeer results for exit
  codes, a missing or directory executable, a missing or non-directory working
  directory, empty and NUL-containing inputs, a batch file (Windows), a signal
  and a non-executable file (POSIX), and states the expected result for two of
  them so a platform where both fail the same way cannot pass. The quoting rules
  have their own cases. The Windows child is `powershell.exe`, because
  `cmd.exe /c` re-parses quotes. The unit test that type-checks `System` for
  every target covers the POSIX file.
- **Not verified.** Struct layout assumptions (`STARTUPINFOW` offsets are
  checked by the successful launch; `posix_spawn_file_actions_t` is given 256
  bytes, enough for glibc and macOS) and the `waitpid` status decoding on a real
  POSIX host.

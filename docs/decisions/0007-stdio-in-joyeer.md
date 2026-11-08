# 0007: writeStderr and readStdinLine in Joyeer

- Status: accepted and implemented (decided and built 2026-11-07). Verified on
  Windows x64 Debug and Release; the POSIX variant is empty and type-checks for
  Linux and macOS.
- Amended by [0011](0011-runtime-without-the-c-library.md): the operations now use the
  runtime's own streams over the operating system instead of the C library's `FILE*`
  streams, `errno` and Windows text-mode switch described below. The documented
  behaviour and error codes are unchanged.
- Spec: [§18.4](../spec/18-host.md#184-standard-error) and
  [§18.5](../spec/18-host.md#185-standard-input), unchanged. The Joyeer
  functions keep every documented behavior, including the C-runtime `errno`
  codes.

## Context

[Decision 0004](0004-remove-host-builtins.md) moves the host operations into
`System`. [The filesystem plan](../plan/filesystem.md) held back `writeStderr`
and `readStdinLine` because a port seemed to need one of two things Joyeer lacks:

- **Windows descriptor protection.** The C code stops the CRT from aborting the
  process on a closed descriptor by installing a thread-local invalid-parameter
  handler, a callback.
- **Read-ahead storage.** `readStdinLine` relies on the CRT's stdin buffer, and
  Joyeer has no mutable global storage.

## Options considered

1. **Win32 handles directly** (`GetStdHandle`, `WriteFile`, `ReadFile`). No
   descriptor crash and no text mode, but the failure codes become
   `GetLastError` values, which changes the documented contract, and stdin would
   need its own read-ahead or one system call per byte.
2. **Keep the C library's streams and port the logic** (chosen). The streams are
   the ones the rest of the program already uses, so buffering, read-ahead and
   `errno` values stay exactly as documented.

## Decision

Option 2, in `stdio.joyeer` (portable logic), `stdio_windows.joyeer` and
`stdio_posix.joyeer` (guards), calling `fwrite`, `fflush`, `ferror`, `clearerr`
and `fgetc` through `@c_extern`.

- **One new runtime accessor**, `joyeer_native_stdio(which)`, returns the `FILE*`
  for standard input, output or error as an integer, because a binding cannot
  name the C globals. It sits beside `joyeer_native_errno()` and
  `joyeer_native_environment()`. It is what lets Joyeer tell end of input from a
  read error (`ferror`), clear a failure (`clearerr`) and flush standard output.
- **The closed-descriptor guard is the Win32 standard handle.** Before changing
  the mode of standard input or error, `stdio_windows.joyeer` requires
  `GetStdHandle` to return a handle that `GetFileType` recognizes. A detached or
  closed stream has none, and the operation returns `EBADF` (9) as the C code
  did. The CRT is never asked to change the mode of a descriptor it would abort
  on.
- **Standard-input binary mode is set on every call**, not once. The C code kept
  a static flag; Joyeer cannot. Setting the same mode again is idempotent and
  also covers a stream re-opened in text mode between calls, so the §18.5
  wording (switched on first use, never restored) still describes the observable
  effect.
- **`errno` is read after the failing call and `EIO` stands in for zero.** Joyeer
  cannot assign `errno = 0` first, so a failure that sets no `errno` would report
  a stale one. The CRT calls used here set it on failure.

## Consequences

- Behavior on Windows and POSIX is that of the C code. The Windows guard was
  checked against descriptors closed with `_close` (not only detached ones): the
  tests close descriptor 0 and 2 and get `EBADF` rather than an abort. A
  descriptor replaced by a different open handle is outside the guarantees, as
  in §18.4.
- `readStdinLine` builds a temporary `[UInt8]` and copies it once into the
  `String`, as the filesystem reader does; the "never more than `maximumBytes`
  plus one byte" bound is kept.
- `fgetc` is called once per byte through the binding. Measure against the C
  loop before any program relies on large piped input.
- **Tests.** `SystemTests` ([stdio.joyeer](../../tests/lib/System/stdio.joyeer))
  runs each scenario through the built-in and through `System`, and also states
  the expected result, so a platform where both fail the same way cannot pass:
  exact bytes with NUL, CR, LF and invalid UTF-8 against a Windows text-mode
  descriptor, an empty write, a read-only descriptor and a closed descriptor for
  standard error, the restored stream mode, line splitting, CR/CRLF removal, the
  limit and resynchronization, an unterminated final line, `EndOfInput`
  repeating, a zero, negative and maximal limit, 128 KiB lines, `0x1A` and NUL,
  the flush of standard output before a read, and a closed standard input. The
  built-in's own check of a closed standard input runs on its first call only, so
  that case runs the Joyeer code alone. The unit test that type-checks `System`
  for every target covers both new platform files.
- **Not verified.** Linux and macOS: the POSIX variant has no platform code,
  only the shared stdio calls.

# Host Operations in Joyeer: Remaining Work

> **Status:** unfinished work only. Every host operation is a Joyeer function in
> `System` and the compiler and C runtime no longer know any of them; see
> [decision 0004](../decisions/0004-remove-host-builtins.md) (removal),
> [0003](../decisions/0003-pure-joyeer-filesystem.md),
> [0006](../decisions/0006-runprocess-in-joyeer.md),
> [0007](../decisions/0007-stdio-in-joyeer.md) and the
> [implemented surface](../impl/supported-features.md). Remove items here when
> they are done.

## 1. Where things stand

- `System` implements the nine §18.2 operations, legacy `readFile`, `runProcess`,
  `writeStderr` and `readStdinLine` in Joyeer, with per-platform primitives in
  `*_windows.joyeer` and `*_posix.joyeer` reached through `@c_extern` bindings.
- Programs write `import System`. fern, moss, the Compression library, the
  end-to-end fixtures and the tests do. The prelude declares only the enumerations
  the operations return (`FileSystemError`, `FileKind`, `ProcessStatus`,
  `ProcessError`, `StderrError`, `StdinError`, `IOError`); keeping them there is
  deliberate, so user code and `System` share one definition.
- Removed: the compiler's host function descriptors and their type checking and
  lowering, `filesystem.c`, `process.c`, and the C `readFile`, `writeStderr` and
  `readStdinLine` ABI functions, together with their C unit tests. What remains
  in the runtime for them is `joyeer_native_errno`, `joyeer_native_environment`,
  `joyeer_native_stdio` and `joyeer_host_valid_utf8` (used by Compression).
- Verified: Windows x64 Debug, through `fern test`, the end-to-end fixtures and the
  unfiltered CTest. **Not verified:** Release, Windows ARM64, Linux and macOS.

## 2. Verify the POSIX variants (blocks claiming Linux and macOS)

Before this change the C implementation was the working POSIX code; now the
Joyeer variants are the only code, and they type-check but have never run. On a
Linux host and a macOS host, run `fern test` for `SystemTests`, the unfiltered
CTest and the fern bootstrap, in Debug and Release. Expect to fix, at least,
the first run:

- struct offsets (`statx` mode at 28, `lstat` mode at 4, `dirent` name at 19 and
  21): add a C test compiled against the real headers that asserts each offset
  and size;
- `readdir_r`, deprecated but exported; if a libc lacks it, the fallback is
  `readdir` plus a way to read the returned pointer, which needs its own
  decision;
- `statx` needs glibc 2.28 or musl with it; decide the minimum libc;
- `errno` after stdio calls, and `EINTR` retries;
- the Linux and macOS differences in `EILSEQ` and `ENAMETOOLONG`;
- `runProcess`: `posix_spawn_file_actions_addchdir_np` availability (glibc 2.29,
  macOS 10.15), the size of `posix_spawn_file_actions_t`, the `waitpid` status
  decoding, and `joyeer_native_environment` against a real `environ`;
- `writeStderr` and `readStdinLine`: `joyeer_native_stdio` against the real
  `stdin`, `stdout` and `stderr`, and the closed-descriptor cases;
- the **expected values** in [tests/lib/System](../../tests/lib/System).
  They state the Windows result and the POSIX result (or alternatives) per
  scenario, and the POSIX ones were written from the code, not observed.

Record the results in [supported-features](../impl/supported-features.md).

**Intel macOS is not covered.** The macOS offsets (`lstat` mode at 4, `d_name` at 21)
are the arm64 layout. On x86_64 the unsuffixed `lstat`, `readdir_r` and `opendir`
symbols are the pre-INODE64 ABI, with different offsets, and `@platform(macos)`
cannot select an architecture. Bind the `$INODE64` symbols or add an
architecture guard before claiming Intel macOS.

## 3. Verify the Compression library and moss

`import System` was added to `compression.joyeer` and to moss's sources, but both
need the optional `JOYEER_BUILD_COMPRESSION` configuration, which a default build
does not enable. Build with it and run `fern test` for the moss modules and
`CompressionTests` before relying on either.

## 4. Settle the allocation contract

The old C reader made one tracked allocation and never held more than the prefix
limit. The Joyeer readers build a temporary `[UInt8]` and copy it once into the
`String`, and `readStdinLine` calls `fgetc` once per byte through the binding.
Either accept the difference and say so in §18.2, or add a copy-free path (for
example, taking ownership of an array's storage as a `String`). Measure first.

## 5. Coverage that moved

The C unit tests for the filesystem, process and stderr/stdin ABIs were removed
with the code. `SystemTests` states results by error case rather than platform
code, so it no longer asserts raw `errno` or `GetLastError` values. Consider a
small number of exact-code assertions per platform once the POSIX variants have
run.

## 6. Not planned

Streaming I/O, atomic replacement, recursive removal, and arbitrary-byte POSIX
paths remain deferred, as in [§18.2](../spec/18-host.md#182-filesystem-operations).

# 0007: Host Operations in Joyeer

- Status: in progress (updated 2026-11-12); implemented, verification remains
- Decisions: [0003](../decisions/0003-pure-joyeer-filesystem.md), [0004](../decisions/0004-remove-host-builtins.md),
  [0006](../decisions/0006-runprocess-in-joyeer.md), [0007](../decisions/0007-stdio-in-joyeer.md)
- Spec: [§18](../../specs/18-host.md)

## Goal

Every host operation is a Joyeer function in `System`, and it is verified on every supported platform.

## Definition of done

- The POSIX variants pass `fern test`, the unfiltered CTest and the fern bootstrap on Linux and macOS,
  in Debug and Release, with the results recorded in [supported features](../impl/supported-features.md).
- The Compression library and moss pass their tests with `import System`.
- The allocation contract of the readers is settled in §18.2.

## Current state

- `System` implements the nine §18.2 operations, legacy `readFile`, `runProcess`, `writeStderr` and
  `readStdinLine` in Joyeer, with per-platform primitives in `*_windows.joyeer` and `*_posix.joyeer`
  reached through `@c_extern` bindings.
- Programs write `import System`; fern, moss, the Compression library, the end-to-end fixtures and the
  tests do. The prelude declares only the enumerations the operations return (`FileSystemError`,
  `FileKind`, `ProcessStatus`, `ProcessError`, `StderrError`, `StdinError`, `IOError`); keeping them
  there is deliberate, so user code and `System` share one definition.
- Removed: the compiler's host function descriptors and their type checking and lowering,
  `filesystem.c`, `process.c`, and the C `readFile`, `writeStderr` and `readStdinLine` ABI functions,
  with their C unit tests. What remains in the runtime for them is `joyeer_native_errno`,
  `joyeer_native_environment`, `joyeer_native_stdio` and `joyeer_host_valid_utf8` (used by Compression).
- Verified on Windows x64 Debug through `fern test`, the end-to-end fixtures and the unfiltered CTest.
  **Not verified:** Release, Windows ARM64, Linux and macOS.

## Todo

### Verify the POSIX variants (blocks claiming Linux and macOS)

The Joyeer POSIX variants type-check but have never run. Expect to fix, at least, the first run.

- [ ] Run `fern test` for `SystemTests`, the unfiltered CTest and the fern bootstrap on Linux, Debug and
  Release.
- [ ] The same on macOS.
- [ ] Add a C test compiled against the real headers that asserts each struct offset and size (`statx`
  mode at 28, `lstat` mode at 4, `dirent` name at 19 and 21).
- [ ] `readdir_r` is deprecated but exported; if a libc lacks it, decide the fallback (`readdir` plus a
  way to read the returned pointer) in its own decision.
- [ ] Decide the minimum libc: `statx` needs glibc 2.28 or musl with it.
- [ ] Check `errno` after stdio calls and `EINTR` retries.
- [ ] Check the Linux and macOS differences in `EILSEQ` and `ENAMETOOLONG`.
- [ ] `runProcess`: `posix_spawn_file_actions_addchdir_np` availability (glibc 2.29, macOS 10.15), the
  size of `posix_spawn_file_actions_t`, the `waitpid` status decoding, and `joyeer_native_environment`
  against a real `environ`.
- [ ] `writeStderr` and `readStdinLine`: `joyeer_native_stdio` against the real `stdin`, `stdout` and
  `stderr`, and the closed-descriptor cases.
- [ ] Confirm the POSIX **expected values** in [tests/lib/System](../../tests/lib/System), which were
  written from the code, not observed.
- [ ] Record the results in [supported features](../impl/supported-features.md).
- [ ] Intel macOS: the macOS offsets (`lstat` mode at 4, `d_name` at 21) are the arm64 layout, and on
  x86_64 the unsuffixed `lstat`, `readdir_r` and `opendir` symbols are the pre-INODE64 ABI.
  `@platform(macos)` cannot select an architecture; bind the `$INODE64` symbols or add an architecture
  guard before claiming Intel macOS.

### Compression and moss

Both need the optional `JOYEER_BUILD_COMPRESSION` configuration, which a default build does not enable.

- [ ] Build with it and run `fern test` for the moss modules.
- [ ] Run `CompressionTests`.

### Allocation contract

The old C reader made one tracked allocation and never held more than the prefix limit. The Joyeer
readers build a temporary `[UInt8]` and copy it once into the `String`, and `readStdinLine` calls
`fgetc` once per byte through the binding.

- [ ] Measure the difference.
- [ ] Either accept it and say so in §18.2, or add a copy-free path (for example, taking ownership of an
  array's storage as a `String`).

### Coverage

The C unit tests for these ABIs were removed with the code. `SystemTests` states results by error case
rather than platform code, so it no longer asserts raw `errno` or `GetLastError` values.

- [ ] Once the POSIX variants have run, consider a few exact-code assertions per platform.

## Open questions

None beyond the decisions named on the todo items.

## Out of scope

Streaming I/O, atomic replacement, recursive removal, and arbitrary-byte POSIX paths remain deferred, as
in [§18.2](../../specs/18-host.md#182-filesystem-operations).

## Progress log

- 2026-11-12: converted to the plan template; no items changed.

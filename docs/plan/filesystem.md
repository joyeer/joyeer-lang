# Filesystem in Pure Joyeer: Remaining Work

> **Status:** unfinished work only. The Joyeer implementation of the §18.2
> operations exists and is verified on Windows x64; see
> [decision 0003](../decisions/0003-pure-joyeer-filesystem.md) and the
> [implemented surface](../impl/supported-features.md). Remove items here when
> they are done.

## 1. Where things stand

- `System` (`src/library/System/filesystem*.joyeer`) implements all nine
  operations in Joyeer: portable logic in `filesystem.joyeer`, per-platform
  primitives in `filesystem_windows.joyeer` and `filesystem_posix.joyeer`, with
  OS calls made through `@c_extern` bindings and the bare byte pointers of
  [§19.4.4](../spec/19-c-interop.md#1944-bare-byte-pointers).
- The compiler's built-in names (`readFileUtf8`, ...) still use the C runtime
  in `src/runtime/filesystem.c`. The Joyeer code is reached as
  `System.readFileUtf8(...)` and is checked against the built-ins by
  [SystemTests](../../tests/library/System/filesystem.joyeer).
- Verified: Windows x64, Debug and Release, through `fern test`. The POSIX
  variants type-check for Linux and macOS in a service test but have not run.

Also in `System`: legacy `readFile(path:)` (`legacy_file.joyeer`), written once
over the C runtime's narrow `fopen` on every platform and compared with the
built-in by the same differential test. [Decision 0004](../decisions/0004-remove-host-builtins.md)
sets the end state: the built-ins are removed and callers `import System`.
[Decision 0005](../decisions/0005-runtime-unsafe-memory.md) sets the direction
for the rest of the runtime.

### Not yet ported: stderr and stdin

`writeStderr` and `readStdinLine` have two problems a faithful port cannot
paper over; each needs a decision before it is written.

- **Windows error codes and descriptors.** The C code protects against a closed
  CRT descriptor by installing a thread-local invalid-parameter *callback*, which
  Joyeer cannot express, and it reports CRT `errno` values
  ([§18.4](../spec/18-host.md#184-standard-error)). A Joyeer port would use the
  Win32 handles directly (`GetStdHandle`, `WriteFile`, `ReadFile`): no text
  mode, no descriptor crash, simpler, but the failure codes become `GetLastError`
  values. That changes the documented contract on Windows.
- **No place to keep read-ahead.** `readStdinLine` relies on the CRT's stdin
  buffer so unread bytes survive between calls. Joyeer has no mutable global
  storage, so a Joyeer reader either reads one byte per system call, or keeps
  calling the CRT's `getchar` and needs a runtime accessor to tell end of input
  from an error and to flush `stdout` first ([§18.5](../spec/18-host.md#185-standard-input)).

`runProcess` is ported ([decision 0006](../decisions/0006-runprocess-in-joyeer.md)):
verified on Windows x64 in Debug and Release; the POSIX variant (`posix_spawn`
with an argument-address table built through the intrinsics) type-checks only.

## 2. Verify the POSIX variants (blocks everything below)

On a Linux host and a macOS host, run `fern test` for `SystemTests` and the
unfiltered CTest, in Debug and Release. Expect to fix, at least, the first run:

- struct offsets (`statx` mode at 28, `lstat` mode at 4, `dirent` name at 19 and
  21): add a C test compiled against the real headers that asserts each offset
  and size;
- `readdir_r`, deprecated but exported; if a libc lacks it, the fallback is
  `readdir` plus a way to read the returned pointer, which needs its own
  decision;
- `statx` needs glibc 2.28 or musl with it; decide the minimum libc;
- `errno` after stdio calls, and `EINTR` retries;
- the Linux and macOS differences in `EILSEQ` and `ENAMETOOLONG`.
- `runProcess`: `posix_spawn_file_actions_addchdir_np` availability (glibc 2.29,
  macOS 10.15), the size of `posix_spawn_file_actions_t`, the `waitpid` status
  decoding, and `joyeer_native_environment` against a real `environ`.

Record the results in [supported-features](../impl/supported-features.md).

**Intel macOS is not covered.** The macOS offsets (`lstat` mode at 4, `d_name` at 21)
are the arm64 layout. On x86_64 the unsuffixed `lstat`, `readdir_r` and `opendir`
symbols are the pre-INODE64 ABI, with different offsets, and `@platform(macos)`
cannot select an architecture. Bind the `$INODE64` symbols or add an
architecture guard before claiming Intel macOS.

## 3. Decide the routing

Today the built-ins win unqualified. Choose, in a decision record:

- **A:** keep the names compiler-known and lower them to the `System`
  functions, loading the standard module implicitly. Source stays compatible.
- **B:** remove the built-ins; callers `import System`. Cleaner, but fern and
  moss change, and [library](library.md) §4's rule that prelude names precede
  imported ones applies.

Either way the `System` module must be available to every program that uses
the names, which depends on the installed-library work in
[library](library.md) §4.

## 4. Settle the allocation contract

The C reader makes one tracked allocation and never holds more than the prefix
limit. The Joyeer reader builds a temporary `[UInt8]` and copies it once into
the `String`. Either accept the difference and say so in §18.2, or add a
copy-free path (for example, taking ownership of an array's storage as a
`String`). Measure first; nothing depends on it today.

## 5. Switch and delete

After sections 2 to 4:

- route the names;
- move [filesystem_test.cpp](../../tests/unittests/runtime/filesystem_test.cpp)
  and the native fixtures to go through the language, then remove the
  `joyeer_fs_*_abi` functions, `filesystem.c`, the `hostbuiltins` entries, and
  their backend lowering;
- point fern and moss at the Joyeer implementation;
- run the unfiltered CTest and `fern test` in Debug and Release on every
  claimed platform, and check that the runtime archive holds no `joyeer_fs_`
  symbol.

## 6. Not planned

Streaming I/O, atomic replacement, recursive removal, and arbitrary-byte POSIX
paths remain deferred, as in [§18.2](../spec/18-host.md#182-filesystem-operations).
Process spawning and standard streams are separate and unchanged.

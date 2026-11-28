# 0011: The runtime without the C library (Tier 2)

- Status: accepted and implemented in stage 10 of the
  [runtime plan](../plan/0008-runtime-in-joyeer.md) (decided and built 2026-11-08). Verified on
  Windows x64 and Linux x86-64 (Ubuntu under WSL2, the full suite), and on
  [Windows ARM64 and Linux AArch64 (WSL2)](../building.md#toolchain-library-layout).
- Spec: [§19.4.5](../../specs/19-c-interop.md#1945-compiler-intrinsics)
  defines the syscall and target-architecture intrinsics.

## Context

[Decision 0009](0009-runtime-core-capabilities.md) put the whole runtime in Joyeer
but left its lowest layer on the platform C library: `malloc`, the stdio streams,
`_Exit`, and the C startup that calls `main`/`wmain`. The plan's Tier 2 (D8 to D10)
removes that layer, so the runtime archive needs only the operating system's
interface. This decision fixes what is replaced, what deliberately stays, and the
behaviour that changes with it.

## Decision

**What stays: the compiler's support symbols.** LLVM may emit calls to `memcpy`,
`memmove`, `memset` and `memcmp` for any code, and the Windows targets also reference
`_fltused` and `__chkstk`. These are the contract between a compiler and its target,
not the C library in the sense of Tier 2. The runtime does not implement them with
Joyeer loops: a loop recognised as a copy could become a call to itself. The symbol
gates allow exactly these names and nothing else from the C library. Windows x64
can use the static CRT helpers without running CRT startup. In the Windows ARM64
own-entry link, the linker instead resolves the memory routines to private wrappers
over kernel32, since the VCRuntime variants can pull in CRT initialization and the
C heap. The stack probe and floating-point marker still come from `libcmt.lib`.

**Per platform.**

| | Allocator | Output and exit | Program entry |
|---|---|---|---|
| Windows | `VirtualAlloc` / `VirtualFree` | `WriteFile`, `GetStdHandle`, `ExitProcess` | own entry `joyeer_start`, unless the program calls the C library or links a native one |
| Linux | raw `mmap` / `munmap` | raw `write`, `read`, `exit_group` | `main` from the C startup |
| macOS | libSystem `mmap` / `munmap` | libSystem `write`, `read`, `_exit` | `main` from dyld |

Linux keeps the C startup: it supplies `main`, `argv` and the dynamic loader's hand-off, and a
own `_start` would buy nothing while the linker still adds libc. `System` on Linux calls
the kernel directly (below), so a program that uses every part of it needs only that
startup and the compiler support functions. macOS `System` calls libSystem. Windows programs that link a native
library (Compression) also need the CRT, so they keep `wmain`.

**D8: the allocator (built in 10.1).** Size classes over memory mapped straight from
the operating system. A request up to 32768 bytes is rounded to a class (sixteen-byte
steps to 512 bytes, then four steps per power of two) and served from the class's free
list or cut from a 1 MiB region; larger requests are their own page-rounded mapping,
unmapped when freed. The sizes `free` and `realloc` receive select the class, so a
block has no header, and a block is always sixteen-byte aligned. Small blocks are never
returned to the system. The allocator is single threaded: the language has no threads,
and the counter stays an atomic add only because it already was. A block moved by
`realloc` is copied with the bulk-memory intrinsic; a block whose class does not change
stays where it is. Rejected: a header holding the size (costs sixteen bytes a block to
protect against a caller bug the tests below find), and a general-purpose coalescing
allocator (slower to write and to verify, no benefit for the runtime's size mix).

A caller that passes a size other than the block's last requested size would corrupt a
free list. During 10.1 the allocator was temporarily built with a size header that
panicked on a mismatch and the whole test suite was run; the callers in the runtime,
`System`, the native helpers and the C interop fixtures all passed correct sizes.

**Linux growth follow-up.** A growing block that must move can try to reserve
2 MiB while live, but only if `RLIMIT_AS` is unlimited and allocator scratch
storage is already available. If the query or reserve fails, it uses an exact
page-rounded allocation. Ordinary allocation still maps the requested size
rounded to pages. The reserved kind keeps a 16-byte offset from the base, so the
size-argument ABI can recover its mapping extent without a size header.
`madvise(MADV_HUGEPAGE)` is a hint, not a requirement, and
`mremap(MREMAP_MAYMOVE)` can preserve pages on subsequent growth, with the
same allocate/copy/free fallback if it fails. No mapping is cached after
release; the allocation counter is unchanged.

**D10: the system-call intrinsic (10.2).** `joyeer_intrinsic_syscall(number, a1, a2,
a3, a4, a5, a6): Int` lowers to inline assembly (`syscall` on x86-64, `svc #0` on
AArch64) and is meaningful only on Linux. It returns the kernel's result
unchanged: a value from -4095 to -1 is `-errno`. Linux system call numbers differ by
architecture, so `joyeer_intrinsic_target_architecture(): Int` returns a constant (1
for x86-64, 2 for AArch64) the runtime selects numbers with. macOS has no supported
system-call interface and calls libSystem; Windows calls `kernel32`.

**R11: output and exit (10.2).** `print` and `System` share one runtime-owned standard
output stream: an 8 KiB buffer mapped directly from the operating system (it is not
an allocation, so the leak count is unchanged), written with `WriteFile` / `write`.
The buffer is flushed when it fills, before a read from standard input, before a
panic message, at program exit, and after every `print` when standard output is a
terminal (checked once with `GetConsoleMode` / `ioctl`), which keeps interactive
programs line-visible as the C library's line buffering did. Standard error is
unbuffered. Each stream keeps the first OS error as an errno-style code
(`EBADF` 9, `EPIPE` 32, `ENOSPC` 28, otherwise `EIO` 5), so `System`'s error values
stay what they were.

Two behaviours change on purpose. A panic now flushes standard output before it
writes its message; with `_Exit` the C library discarded buffered output, so a program
that printed and then failed lost the text when its output was a pipe. Exit status and
the message are unchanged. And a program that is killed leaves unflushed output
behind, as with the C library.

**D9: the entry point (10.3).** On Windows the entry object also defines `joyeer_start`.
The linker reads the program object's undefined symbols: when no native library is selected
and every one is the runtime's own (`joyeer_`), a compiler support symbol, or a Win32 API
name, the link passes `/ENTRY:joyeer_start` and links the runtime archive by symbol. It
adds `libvcruntime.lib` for memory helpers on x64, or supplies weak aliases to the
private Joyeer wrappers on ARM64. Win32 names are PascalCase and the C
library's are lower case or underscored, so a name that does not start with a capital is taken
for the C library: a wrong guess only keeps the C startup, which is always correct. The C
`wmain`/`main` wrappers (and their `atexit` call) are a third archive object, so that link
leaves them out. `joyeer_start` reads `GetCommandLineW`, splits it with the rules the CRT
and `CommandLineToArgvW` use (backslashes are literal unless they precede a quote; `"` toggles
quoting; two quotes inside a quoted part give one), converts each argument to UTF-8 as before,
runs the shared entry logic and calls `ExitProcess` with the status. The argument memory is
mapped directly from the system and kept for the life of the process. With a native library,
or any C library call, the link keeps the CRT startup and `wmain`. A program that needs
no C library still imports only OS functions.

**R12: `System` (10.4).** `System`'s standard error and standard input stop using C
streams: they call the runtime's stream functions (`joyeer_stdout_flush`,
`joyeer_stderr_write`, `joyeer_stdin_read_byte`), so the buffering above is the only
buffering. The whole-file legacy reader on Windows uses `CreateFileA` and `ReadFile`
and maps `GetLastError` to the same errno values the C library did (its `_dosmaperr`; an empty path
is EINVAL as before). Windows `System` then calls `kernel32` only, which the tests check by the
imports of programs that use each part.

**POSIX `System`.** On Linux the file, directory and process operations are system calls
(`filesystem_linux.joyeer`, `process_linux.joyeer`): `openat` (always close-on-exec),
`read`, `write`, `mkdirat`, `unlinkat`, `getdents64`, `statx`, `getcwd`, and for a process
`pipe2`, `clone` with only `SIGCHLD` (a fork), `chdir`, `execve` and `wait4`. A failed `chdir`
or `execve` in the child is reported to the parent through the close-on-exec pipe as the
errno of the step, as `posix_spawn` did, and the child leaves with `exit_group`. The
environment for `execve` is the block after `argv`, whose address the entry keeps in static
slot 16. A file descriptor opened by `System` is no longer inherited by a child (the C
streams were), which is the safer behaviour. macOS keeps its libSystem calls (there is no
stable system-call ABI to use). File I/O uses `openx_np` with `O_CLOEXEC`,
`read`, `write`, and `close`; it does not need a `FILE *`. The tests check with
`llvm-nm` that a Linux program using each part of
`System` needs only `__libc_start_main` and its startup companions and `memcpy`, `memset`,
`memmove` and `memcmp`.
`joyeer_stdin_discard` drops buffered input for a program, or a test, that redirects its own.

## Consequences

- No `malloc`, `fwrite`, `fputc`, `_write`, `_Exit` or stdio data symbol remains in the
  runtime objects; the symbol gates list the system calls instead.
- Allocation-heavy programs are the likely place for a regression: the allocator is
  new and untuned. The runtime benchmark covers it.
- Programs observe one behaviour change (panic flushes standard output) and otherwise
  the same output, ordering and exit codes.
- A program with the C startup registers the flush with `atexit`, so a binding to the C
  `exit` loses nothing; a program on the own entry point has no `exit` to call. A killed
  process loses unflushed output, as with the C library.

## Evidence

The [platform gates and performance status](../plan/0008-runtime-in-joyeer.md) distinguish
native execution from cross-target inspection. The [Linux AArch64 R0 comparison](../../tests/perf/README.md#r0-on-linux-aarch64)
exceeds the performance limit on several rows. The library `Array` and `Dict` still call
the measured ABI, so those rows are not exempt merely because the types are now library code.

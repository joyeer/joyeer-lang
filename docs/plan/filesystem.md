# Filesystem in Pure Joyeer: Plan

> **Status:** proposed; nothing here is implemented. Current behavior:
> [spec §18](../spec/18-host.md), [runtime](../impl/runtime.md),
> [`src/runtime/filesystem.c`](../../src/runtime/filesystem.c). Platform
> selection (`@platform`, [§11.5](../spec/11-annotations.md#115-platform)) is
> implemented. Remove completed work from this plan.

## 1. Goal

Implement every operation in [§18.2](../spec/18-host.md#182-filesystem-operations)
in Joyeer, in the `System` library, calling the operating system's libraries
directly through `@c_extern`. When finished, the 597-line `filesystem.c`, the
`joyeer_fs_*_abi` entry points, and their compiler/backend lowering are gone.

"Pure" means no filesystem-specific C. The C that remains is generic runtime
substrate (allocation, startup, panic, and the native-error slot in §3, E5),
which knows nothing about files or paths.

**Verdict.** Today's FFI cannot express these calls (§2), but the gap is five
small, generic extensions plus a few byte-buffer library functions (§3). None
needs raw memory loads: out-structures are filled into caller-supplied byte
buffers and read back with ordinary indexing.

**Non-goals:** changing §18 semantics, error tags, or encoding rules; a public
or general FFI (extensions stay behind the existing toolchain authorization,
[§19.1](../spec/19-c-interop.md)); streaming, recursive removal, atomic
replacement, arbitrary-byte POSIX paths; changing legacy `readFile`.

## 2. Why It Cannot Be Written Today

[c-interop](../impl/c-interop.md) admits scalars and four buffer forms.

| Needed by an OS call | Gap |
|---|---|
| `open(const char*, int, ...)`, `mkdir(path, mode)` | A borrowed `String` becomes `(ptr, count)` with no NUL; the count lands in `flags`/`mode` |
| `CreateFileW(const wchar_t*, ...)` | No UTF-16 NUL-terminated form |
| `open` with `O_CREAT` | Variadic; unsupported (and Apple ARM64 passes variadic arguments on the stack) |
| `errno` | Needs a pointer from `__errno_location()` and a dereference |
| `read(fd, buf, n)`, `ReadFile(h, buf, n, &got, NULL)` | No mutable buffer; no pointer-only out-parameter |
| `opendir`, `FindFirstFileW` handles | `Int` is documented as not an address carrier |
| Closing every handle on every path | No user `deinit` |

## 3. Required Extensions

Each needs a decision record, spec text in §19, failing tests first, and a
C fixture in `tests/c-interop/` on every claimed platform.

| ID | Extension | Used for |
|---|---|---|
| E1 | `CString` and `WString` adapters: a borrowed `String` passed as one NUL-terminated `char*` (UTF-8) or `wchar_t*` (UTF-16). The runtime copies, applies the strict UTF-8 and embedded-NUL checks that `joyeer_host_string` applies today, and frees after the call | every path argument |
| E2 | Mutable byte buffers: `inout [UInt8]` as `(ptr, count)` for `read`/`write`/`ReadFile`/`WriteFile`, and a pointer-only form (no count) for fixed-size out-structures and `DWORD*` results | read loops, `statx`/`lstat`, `FindFirstFileW`, `readdir_r`, `GetFileInformationByHandle` |
| E3 | Opaque native handles: allow `Int` for values the native side owns (fd, `HANDLE`, `DIR*`, `FindFirstFile` handle) and document that Joyeer never dereferences or does arithmetic on them. Amend §19.4's "not an address carrier" | handles |
| E4 | Variadic calls with a declared fixed-argument count, e.g. `@c_extern(symbol: "open", fixed: 2)`, lowered as an LLVM varargs call so each target's ABI applies | `open` |
| E5 | Captured native error: a binding option that stores `errno` / `GetLastError()` into a runtime slot immediately after the call, before temporaries from E1 are released, plus `nativeError(): Int` to read it | all error reporting |

Library prerequisites (Joyeer, not FFI). These were not found in the
implemented surface; confirm before F2:

- A zeroed `[UInt8]` of length N, trimming or copying a range, and appending a
  byte range to an array, all in linear time.
- A linear-time `String` from `[UInt8]` (byte-at-a-time concatenation is
  quadratic).
- UTF-16LE to UTF-8 decoding with strict surrogate checking, in `Foundation`.

Struct layouts are selected per operating system, not per architecture, by
using layouts that are the same on both supported architectures: `statx` on
Linux (`stx_mode` at offset 28), `struct stat` on macOS (`st_mode` at offset 4),
`WIN32_FIND_DATAW` on Windows. A C test compiled against the real headers
asserts every offset and size used.

## 4. Module Shape

`System` gains two internal layers. Public signatures are exactly the §18.2
declarations; nothing about the `FileSystemError` or `FileKind` shapes changes.

- **Portable layer** (one source): path validation, lexical `joinPath`, error
  classification, read/write loops with the prefix limit, result assembly.
- **Primitive layer** (`@platform` variants with identical signatures): open,
  read, write, close, make/remove, kind, and directory iteration over `Int`
  handles. Shared logic never branches on the OS.

| Operation | Windows primitives | POSIX primitives |
|---|---|---|
| read file / prefix | `CreateFileW`, `ReadFile` loop, `CloseHandle` | `open(O_RDONLY)`, `read` loop (retry `EINTR`), `close` |
| `writeFileNew` | `CreateFileW(CREATE_NEW)`, `WriteFile` loop | `open(O_WRONLY\|O_CREAT\|O_EXCL, 0666)`, `write` loop |
| `createDirectory` | `CreateDirectoryW` | `mkdir(path, 0777)` |
| `listDirectory` | `FindFirstFileW`, `FindNextFileW`, `FindClose`; decode `cFileName` (offset 44) | `opendir`, `readdir_r` into a buffer (`d_name` at 19 on Linux, 21 on macOS), `closedir` |
| `fileKind` | `GetFileAttributesW`; metadata-only `CreateFileW` with `FILE_FLAG_OPEN_REPARSE_POINT`, `GetFileType`, `GetFileInformationByHandle` for devices | Linux `statx(AT_SYMLINK_NOFOLLOW)`; macOS `lstat`; read the mode field |
| `removeFile` | `GetFileAttributesW`, `DeleteFileW` or `RemoveDirectoryW` for links | `unlink`, then a kind query to report `IsDirectory` |
| `removeDirectory` | `GetFileAttributesW`, `RemoveDirectoryW` | kind query, `rmdir` |

Error classification is a per-OS function mapping native codes to the seven
`FileSystemError` tags, ported from `fsError`. Constants differ (`EILSEQ` is 84
on Linux and 92 on macOS; `ENAMETOOLONG` is 36 and 63), so they live in the
`@platform` variants.

## 5. Behavior That Must Not Change

These are easy to lose in a rewrite; each needs a test in the existing fixtures:

- Strict UTF-8, nonempty, no embedded NUL for every path; rejected input
  reports the platform's invalid-input code.
- Exclusive create enforced by `O_EXCL` / `CREATE_NEW`, never by a prior check.
- `readFilePrefix`: negative limit returns `Other(INVALID_PARAMETER/EINVAL)`
  before path resolution; zero limit still opens and closes; no EOF probe at
  the limit; read or close failure discards partial contents.
- First error wins; a close failure is reported only if nothing failed earlier.
- `listDirectory` fails, never skips, on a name that is not valid UTF-8 (or
  invalid UTF-16); `.` and `..` omitted; an empty directory is success.
- Windows drive-relative and bare-drive `joinPath` rules; `Symlink`/`Other`
  classification of reparse points and DOS devices.
- Directory-vs-denied disambiguation on open and on remove.

## 6. Handles Without `deinit`

Handles are `Int` and closed by hand. Rules, checked in review:

- One open site and one close site per operation; the code between them is a
  separate function that returns a `Result` and never returns early past the
  close.
- The caller merges the work error and the close error (first wins).
- Directory iteration closes the handle on every exit and destroys a partial
  result array on failure.

When noncopyable resources exist ([hybrid-runtime](hybrid-runtime.md) §8),
replace this convention with a resource type.

## 7. Routing the Builtins

The compiler lowers the filesystem names to `joyeer_fs_*_abi`
([hostbuiltins.h](../../src/compiler/frontend/hostbuiltins.h)). After the
migration those names must reach the `System` code. Decide in a record before
F5:

- **A:** keep the names compiler-known and lower them to `System` functions,
  loading the standard module implicitly. Source stays compatible.
- **B:** remove the builtins; callers `import System`. Cleaner, but
  [fern](../../tools/fern) and [moss](../../tools/moss) change, and library
  §4's rule that prelude names precede imported ones applies.

## 8. Phases

| Phase | Deliverable and exit criteria |
|---|---|
| F0: Baseline | Inventory each `joyeer_fs_*` entry point's contract, allocation counts, and error results. Move [filesystem_test.cpp](../../tests/unittests/runtime/filesystem_test.cpp) coverage to fixtures that go through the language |
| F1: FFI extensions | Decisions and spec for E1 to E5; implementation through parser, type checker, IR verifier, and backend; C fixtures per platform, including Apple ARM64 variadic stack passing and error capture before temporary release |
| F2: Byte library | The §3 library prerequisites in `Foundation`, with tests |
| F3: Primitive layers | Windows and POSIX primitive layers behind `@platform`, plus a header-conformance C test asserting prototypes, sizes, and offsets against the real headers |
| F4: Portable layer | Validation, `joinPath`, classification, loops, and results. Differential tests run this implementation and the C one on a shared corpus while both exist |
| F5: Switch | Routing decision applied; `filesystem.c`, the ABI header, `hostbuiltins` entries, backend lowering, and ABI tests removed; fern and moss updated |
| F6: Acceptance | §9 on every claimed platform |

F3's two layers can proceed in parallel once F1 and F2 land.

## 9. Acceptance

Final gate is default builds plus unfiltered CTest with unit tests and fern
enabled ([building](../building.md)).

- Every §18 contract and every error tag and native code is unchanged on
  Windows and POSIX; the differential corpus shows identical results.
- No `joyeer_fs_` symbol and no filesystem-specific C remains in the runtime
  archive.
- Failure injection at every step of every operation shows no leaked fds or
  handles and a zero allocation balance.
- Allocation: the C reader makes one tracked allocation and never exceeds the
  prefix limit. Measure the Joyeer reader against that baseline. Either match
  it, or relax the contract in §18.2 through a decision record. Do not silently
  weaken it.
- Prototype and layout conformance tests pass against the real system headers.
- The `System` filesystem code never calls a filesystem builtin.
- Fresh native Debug and Release results per platform; one host does not
  certify another. State minimum libc versions (`statx` needs glibc 2.28).

## 10. Risks

- **No prototype checking.** The compiler cannot compare a binding to the real
  C prototype ([c-interop](../impl/c-interop.md) §3). A wrong signature is a
  bug on one OS only. The F3 conformance test is the mitigation, not a proof.
- **`readdir_r` is deprecated** on glibc and macOS, though still exported. If
  it is removed or misbehaves, the fallback is one raw-memory load primitive to
  read the `readdir` result. That is an unsafe feature and needs its own
  decision.
- **libc variety on Linux.** glibc and musl differ in available symbols and
  struct layouts; the supported set must be written down.
- **Platform quirks move into Joyeer.** The device and reparse-point handling
  on Windows is the most intricate part of today's C.
- **Review burden.** Raw OS calls written in a language without pointer types
  are checked by tests and review, not by the compiler.

## 11. Open Questions

- Spelling of E1/E2 in source: wrapper types (`CString`, `WString`),
  access-keyword variants, or an annotation argument.
- E5 as a per-binding option reading a runtime slot, versus an accessor that
  returns the error as an extra result.
- Routing: A or B in §7.
- Allocation contract: match C exactly, or relax §18.2.
- Whether `statx`/`lstat` and `FindFirstFileW` buffers should come from a
  shared fixed-size buffer helper.

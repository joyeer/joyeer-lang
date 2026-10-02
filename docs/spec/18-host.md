## §18 Portable host operations

This chapter defines the accepted first filesystem, synchronous-process, and
standard-error contracts. See [the implemented surface](../impl/supported-features.md) for
implementation status. These compiler-known operations do not introduce
source-language FFI or user-defined generic functions.

### 18.1 Encoding, paths, and ownership

File contents remain arbitrary bytes in `String`, including embedded NUL.
Paths and process arguments at the new host boundary must instead be valid
UTF-8 without embedded NUL. Invalid input is reported, never replaced.
Windows converts this input to UTF-16 and uses wide-character host APIs.
POSIX preserves valid UTF-8 bytes; non-UTF-8 POSIX paths and arguments are
outside this first portable interface.

Paths must be nonempty. Relative paths are interpreted against the calling
process's current directory, not the program's installation directory.
Path syntax otherwise follows the host: UTF-8 encoding does not make Windows
drive paths meaningful on POSIX. Empty individual process arguments are valid.

Inputs are borrowed and evaluated once in declaration order. Successful
strings and arrays are independently owned values with ordinary deterministic
cleanup. None of these operations changes the parent's working directory.

### 18.2 Filesystem operations

The signatures below describe compiler-provided functions:

```joyeer
func readFileUtf8(path: String): Result<String, FileSystemError>
func readFilePrefix(path: String, maximumBytes: Int): Result<String, FileSystemError>
func writeFileNew(path: String, contents: String): Result<Void, FileSystemError>
func createDirectory(path: String): Result<Void, FileSystemError>
func listDirectory(path: String): Result<[String], FileSystemError>
func fileKind(path: String): Result<FileKind, FileSystemError>
func removeFile(path: String): Result<Void, FileSystemError>
func removeDirectory(path: String): Result<Void, FileSystemError>
func joinPath(base: String, path: String): Result<String, FileSystemError>
```

`FileKind` has payloadless cases `File`, `Directory`, `Symlink`, and `Other`.
`FileSystemError` has `InvalidPath(Int)`, `NotFound(Int)`,
`PermissionDenied(Int)`, `AlreadyExists(Int)`, `NotDirectory(Int)`,
`IsDirectory(Int)`, and `Other(Int)`. The payload preserves the original
platform code (or the platform's invalid-input code for rejected input).
`Other` is an explicit failure category, not an apparent success.

- `readFileUtf8` reads a whole file and preserves its bytes. The `Utf8` suffix
  describes the path, not the file's contents.
- `readFilePrefix` reads from the beginning until EOF or `maximumBytes` bytes
  have been read, whichever occurs first. The result preserves arbitrary bytes,
  including NUL and invalid UTF-8, and can end inside a UTF-8 sequence. A zero
  limit still validates and successfully opens and closes the path before
  returning an owned empty string. A negative limit returns
  `.Err(.Other(code))` with `ERROR_INVALID_PARAMETER` on Windows or `EINVAL`
  on POSIX, before path resolution; it does not add an error case or classify
  the limit as `InvalidPath`. Read and close failures discard partial contents.
  Content storage grows only up to the limit, with fixed empty-string/native
  bookkeeping overhead and separate path conversion storage, never according
  to the whole file's size. Reaching the limit is success and does not probe
  another byte or report whether EOF was reached. For a 65,536-byte manifest
  ceiling, request `maximumBytes: 65537` and reject a returned byte count above
  65,536. This is a bounded first interface, not streaming or generic I/O.
- `writeFileNew` creates a new file exclusively and writes the supplied bytes.
  An existing destination is not truncated or replaced. Exclusivity must be
  enforced by creation itself, not by an earlier existence check. Failure
  after creation can leave a partial new file; this operation is not an
  atomic-publication guarantee.
- `createDirectory` creates exactly one directory. An existing destination or
  missing parent is an error; it does not recursively create parents.
- `listDirectory` returns names, not full paths, and omits `.` and `..`.
  Enumeration order is unspecified. Names that cannot be represented by this
  UTF-8 interface cause failure, not omission or replacement. The result is
  owned, but enumeration is not an atomic snapshot of concurrent changes.
- `fileKind` inspects the final entry without following a symbolic link.
  Platform reparse entries that must not be treated as ordinary files or
  directories are reported as `Symlink` or `Other`.
- `removeFile` removes a file or the final link itself, not a directory tree
  or a link's target.
- `removeDirectory` removes an empty directory only. It never recursively
  removes contents or follows a final link into a target directory.
- `joinPath` joins paths lexically, using host separators. An absolute second
  path replaces the base. On Windows, a drive-relative second path such as
  `C:child` also replaces the base and remains drive-relative; appending it to
  an unrelated base would change its meaning. Both operands must be nonempty.
  A bare drive base such as `C:` stays drive-relative (`C:` plus `child`
  produces `C:child`, not `C:\child`); directory enumeration preserves the
  same drive-current-directory meaning.
  The operation neither accesses the filesystem nor collapses `..`, resolves
  links, or proves confinement.

Ordinary host resolution can follow links in ancestor components; reading
and enumeration can follow their final component as well. These operations
are not a sandbox or a race-free capability interface. Code must not infer
confinement from string prefixes, `joinPath`, or a preceding `fileKind` check.
Recursive cleanup is not exposed in this first interface. A future cleanup
operation for tool-owned outputs must anchor traversal and not follow links;
an unsafe check-then-recurse substitute is not part of this contract.

Replacement, atomic replacement, canonicalization, streaming, arbitrary-byte
POSIX paths, and recursive cleanup are explicitly deferred.

#### Compatibility

Existing `readFile(path:) -> Result<String, IOError>` is unchanged, including
its narrow Windows CRT path behavior. `FileSystemError` is a separate type;
no cases are added to `IOError`, preserving existing exhaustive matches.
Adapters and error wrapping use explicit `match`; `?` does not implicitly
convert between these error types.

### 18.3 Synchronous processes

```joyeer
enum ProcessStatus {
    Exited(Int),
    Signaled(Int),
}

enum ProcessError {
    InvalidInput(Int),
    NotFound(Int),
    PermissionDenied(Int),
    LaunchFailed(Int),
    WaitFailed(Int),
}

func runProcess(
    executable: String,
    arguments: [String],
    workingDirectory: String
): Result<ProcessStatus, ProcessError>
```

`arguments` excludes the executable name. The operation launches one child
and waits for completion before returning. It inherits the parent's
environment and standard input, output, and error.

An explicit executable path is resolved relative to the parent's working
directory before changing the child's directory. Executable lookup on PATH
is not performed, including for a bare relative filename. The child's working
directory is an explicit nonempty path, also relative to the parent when not
absolute. The parent's directory remains unchanged.

No shell, variable expansion, wildcard expansion, or command-string
evaluation is implied. Empty arguments, spaces, quotes, backslashes, Unicode,
and literal shell metacharacters retain their argument boundaries. Windows
argument serialization follows the Microsoft C runtime convention; it does
not promise equivalence for arbitrary private command-line parsers. Windows
batch files are not implicitly run through a command interpreter.

A nonzero exit is `.Ok(.Exited(code))`, not a launch failure. The full native
Windows unsigned exit status fits in `Int` and is preserved, even outside
`0..255`. POSIX signal termination produces `.Ok(.Signaled(signal))`.
Launch and wait failures instead return `.Err` with a category and original
platform code. If the operation must return an error after creating a child,
it must clean up the child and native resources rather than leave an
unreported orphan or zombie.

There is no cancellation API in this first synchronous interface. Native
signal and console-control behavior applies; cleanup after external forced
termination is not guaranteed. Pipes, output capture, timeout policies,
asynchronous handles, and shell-script interpretation are deferred.

#### Project-manager status policy

The planned `joypm run` maps successful child completion to its own status
zero, unsuccessful completion or launch/wait failure to one, and invalid CLI
usage to two. It reports the original child status or platform error rather
than truncating it to fit its own entry result. This tool policy is detailed in
the [package manager plan](../plan/package-manager.md#3-cli-design-and-status-conventions);
it is not a restriction on language `ProcessStatus` or a claim that `joypm` is
implemented.

### 18.4 Standard error

```joyeer
enum StderrError {
  WriteFailed(Int),
}

func writeStderr(contents: String): Result<Void, StderrError>
```

`contents` is borrowed and evaluated once. The operation writes exactly its
bytes to the inherited standard-error stream, without appending a newline,
validating UTF-8, replacing invalid bytes, or treating embedded NUL as a
terminator. Windows CRT text-mode newline translation is disabled for this
write; the previous stream mode is restored afterward. No console code page
is changed. Terminal rendering is a host concern, not an encoding guarantee.

The operation flushes standard error before returning success, even when
`contents` is empty. Success is `.Ok(())`. Write and flush failures return
`.Err(.WriteFailed(code))`, where `code` is the original native C-runtime
`errno`, or `EIO` if the failed operation supplies no code. Windows stream-mode
setup/restoration failures use the same error case. These are C-runtime codes,
not Win32 `GetLastError` values. An error can follow a partial write; no rollback,
atomic-message publication, or filesystem durability is promised. Native
signal behavior still applies, including POSIX `SIGPIPE` on a closed pipe.

`StderrError` is distinct from `IOError`, `FileSystemError`, and `ProcessError`.
Postfix `?` propagates only the same error type; adapters must explicitly match
and wrap errors. This operation does not change `print` or the compatibility
`readFile` interface. Concurrent foreign code that changes the CRT stream mode
or reopens standard error is outside this first interface's guarantees.

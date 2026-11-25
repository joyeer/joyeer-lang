## §18 Portable host operations

This chapter defines filesystem, process, standard-stream, host-query,
clock, and owned-resource contracts, and the provisional TCP
networking contract (§18.6). See [the implemented surface](../docs/impl/supported-features.md) for
implementation status. These operations are functions of the `System` standard
module (`import System`, §12.5); they are not compiler-known and do not introduce
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

Inputs are borrowed unless marked `inout` or `consuming`, and evaluated once
in declaration order. Successful
strings and arrays are independently owned values with ordinary deterministic
cleanup. None of these operations changes the parent's working directory.

`System.isWindows(): Bool` returns whether the compilation target is Windows.
It performs no runtime host lookup. Lexical path helpers with a `windows: Bool`
argument still use the caller's choice of path syntax.

### 18.2 Filesystem operations

The signatures below are public functions of `System`:

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
  Each logical content read requests at most the remaining byte budget, and
  any accumulating buffer holds only bytes accepted by the read. Reaching the
  limit is success and neither triggers an extra read to check for EOF nor
  reports whether EOF was reached. Host buffering may read ahead.

  Live storage for file contents, including the owned result, is bounded by
  fixed overhead plus space proportional to the bytes accepted; it does not
  grow with the unread remainder of the file. The implementation may use
  fixed-size I/O scratch, rounded-up buffer capacity, a separate result copy,
  and separate path and host bookkeeping. The limit does not guarantee exact
  capacity, an allocation count, or zero-copy behavior. For a 65,536-byte manifest
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

Replacement, atomic replacement, canonicalization, arbitrary-byte POSIX paths,
and recursive cleanup are deferred. See §18.9 for sequential owned handles.

#### Compatibility

Existing `readFile(path:) -> Result<String, IOError>` is unchanged, including
its narrow Windows CRT path behavior. `FileSystemError` is a separate type;
no cases are added to `IOError`, preserving existing exhaustive matches.
Adapters and error wrapping use explicit `match`; `?` does not implicitly
convert between these error types.

### 18.3 Child processes

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
func startProcess(
    executable: String,
    arguments: [String],
    workingDirectory: String
): Result<ChildProcess, ProcessError>
func waitProcess(child: consuming ChildProcess): Result<ProcessStatus, ProcessError>
```

`arguments` excludes the executable name. `runProcess` launches one child
and waits for completion before returning. `startProcess` uses the same
launch rules and returns an owned `ChildProcess` once launch succeeds,
without waiting for completion. The child inherits the parent's environment
and standard input, output, and error.

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

`waitProcess` consumes the owner and blocks until the child finishes; it
returns the same status as `runProcess`. Interrupted POSIX waits are retried.
Dropping a `ChildProcess` on normal destruction also waits and reaps, ignoring
its status. This can block indefinitely, including if the child needs work
from the parent; prefer an explicit wait. Neither action implicitly kills or
detaches the child. An error may be returned when there is no remaining
ownership obligation. Failure to discharge a live child's ownership during
wait is a fatal invariant failure, not a way to detach it. External reaping
or auto-reaping of the owned child is outside this contract.

There is no polling, cancellation, or detach API. Native signal and
console-control behavior applies; cleanup after a trap or external forced
termination is not guaranteed. Pipes, output capture, timeouts, and
shell-script interpretation remain deferred. See §21.6 for the task restriction.

#### Project-manager status policy

`fern run` maps successful child completion to its own status
zero, unsuccessful completion or launch/wait failure to one, and invalid CLI
usage to two. It reports the original child status or platform error rather
than truncating it to fit its own entry result. This tool policy is detailed in
[fern status and diagnostics](../docs/impl/fern.md#status-and-diagnostics);
it is not a restriction on the language's `ProcessStatus` contract.

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

### 18.5 Standard input

```joyeer
enum StdinError {
    EndOfInput(Int),
    LineTooLong(Int),
    ReadFailed(Int),
}

func readStdinLine(maximumBytes: Int): Result<String, StdinError>
```

`readStdinLine` blocks until one complete line, the end of the standard-input
stream, or a read failure. It returns an owned
`String` result.

- A line ends at `\n`. The terminator is not part of the result. One `\r`
  immediately before the `\n` is also removed. All other bytes, including NUL
  and invalid UTF-8, are returned unchanged.
- Stream end after at least one byte is a final unterminated line, returned as
  a success and not stripped of a trailing `\r`. Stream end with no bytes
  before it returns `.Err(.EndOfInput(0))`, and every later call does too. An
  empty line terminated by `\n` is a successful empty `String`, distinct from
  `EndOfInput`.
- `maximumBytes` bounds the returned line after terminator removal. It must be
  at least zero. The runtime never holds more than `maximumBytes` plus one
  byte of the line, whatever the peer sends.
- A longer line returns `.Err(.LineTooLong(0))`. The runtime first discards the
  rest of that line through its terminator (or the stream end), so the next
  call starts at the following line.
- A negative limit, or an underlying read failure, returns
  `.Err(.ReadFailed(code))`, where `code` is the native C-runtime `errno`
  (`EINVAL` for a negative limit, or `EIO` if the failed read supplies none). A
  failed read discards any partial line.
- `StdinError` payloads are `Int` like the other host errors; `EndOfInput` and
  `LineTooLong` carry 0.

On Windows the process's standard input is switched to binary mode on first
use, so CRLF, `0x1A`, and other bytes are not translated and `0x1A` does not
end the stream. The change is process-wide and is not restored. Reads are not
atomic across threads, there is no timeout or cancellation, and mixing this
operation with `runProcess` children that read the same standard input is
unspecified.

Before blocking for input, `readStdinLine` flushes standard output, so a peer
waiting for a prompt or response sees everything printed so far. A flush
failure is ignored, because `print` has no error path. The flush also happens
before a read that ends in `EndOfInput`. A negative limit, or a failure to set
up standard input, returns before any flush. Output is not flushed at any other
point.

### 18.6 TCP networking

> **Status: provisional.** Basic IPv4 TCP passed Windows x64 and Linux x86-64
> testing. The newer poll and deadline APIs have native Windows x64 and
> macOS ARM64 coverage. Linux still needs native acceptance for these APIs. See
> [0012](../docs/decisions/0012-tcp-networking.md) and
> [0014](../docs/decisions/0014-network-timeouts.md). The read readiness queue
> has its own provisional contract below.

```joyeer
struct TcpListener { /* representation unspecified */ }
struct TcpConnection { /* representation unspecified */ }
struct TcpReadQueue { /* representation unspecified */ }

enum NetworkError {
    InvalidInput(Int),
    AddressInUse(Int),
    AddressNotAvailable(Int),
    PermissionDenied(Int),
    ConnectionRefused(Int),
    ConnectionReset(Int),
    TimedOut(Int),
    Other(Int),
}

func tcpListen(address: String, port: Int, backlog: Int): Result<TcpListener, NetworkError>
func tcpAccept(listener: TcpListener, timeoutMillis: Int): Result<TcpConnection, NetworkError>
func tcpConnect(address: String, port: Int): Result<TcpConnection, NetworkError>
func tcpConnectWithin(address: String, port: Int, timeoutMillis: Int): Result<TcpConnection, NetworkError>
func tcpLocalPort(listener: TcpListener): Result<Int, NetworkError>
func tcpPoll(connections: [TcpConnection], listeners: [TcpListener], timeoutMillis: Int): Result<TcpReadiness, NetworkError>
func tcpOpenReadQueue(connections: consuming [TcpConnection]): Result<TcpReadQueue, NetworkError>
func tcpReadQueueWait(queue: inout TcpReadQueue, timeoutMillis: Int): Result<Int?, NetworkError>
func tcpReadQueueReceive(queue: inout TcpReadQueue, maximumBytes: Int): Result<String?, NetworkError>
func tcpReadQueueSendWithin(queue: inout TcpReadQueue, contents: String, timeoutMillis: Int): Result<Void, NetworkError>
func tcpReadQueueCloseCurrent(queue: inout TcpReadQueue): Result<Void, NetworkError>
func tcpReceive(connection: TcpConnection, maximumBytes: Int, timeoutMillis: Int): Result<String, NetworkError>
func tcpSend(connection: TcpConnection, contents: String): Result<Void, NetworkError>
func tcpSendWithin(connection: inout TcpConnection, contents: String, timeoutMillis: Int): Result<Void, NetworkError>
func tcpCloseConnection(connection: consuming TcpConnection): Result<Void, NetworkError>
func tcpCloseListener(listener: consuming TcpListener): Result<Void, NetworkError>
```

These are public functions of `System`. Arguments are borrowed unless the
signature says otherwise, and evaluated once in declaration order. Every
successful `String` is independently owned.
`TcpListener`, `TcpConnection`, and `TcpReadQueue` own native resources. Each
declares a `deinit` (§3.7), so it is noncopyable (§4.1.1): a program cannot copy
it into a second owner, and a `Result` or `Optional` that holds one is
noncopyable too. Their storage is not public.

#### Addresses, ports, and limits

- `address` is an IPv4 dotted quad of four decimal fields, each `0` to `255`,
  with no leading zeros except the single digit `0`, and no signs, spaces,
  or other characters. Anything else, including host names and IPv6 text,
  returns `.Err(.InvalidInput(code))`, where `code` is `EINVAL` on POSIX or
  `WSAEINVAL` on Windows. The text is parsed in Joyeer; no resolver runs.
- `"0.0.0.0"` listens on every IPv4 interface and `"127.0.0.1"` on loopback
  only. Choosing the wider address is the program's decision; no operation
  defaults to it.
- `port` is `0` to `65535`. For `tcpListen`, `0` asks the system for an unused
  port, which `tcpLocalPort` reports. For `tcpConnect`, `0` is invalid.
  A value outside the range is `.InvalidInput`.
- `backlog` must be at least `1`. A value larger than the host allows is
  reduced to the host maximum, not an error.
- `maximumBytes` for `tcpReceive` is `1` to `1048576` (1 MiB). Another value is
  `.InvalidInput`, checked before any system call.
- `timeoutMillis` for `tcpAccept` and `tcpReceive` is `-1`, `0`, or `1` to
  `2147483647`; see Timeouts below. Another value is `.InvalidInput`, checked
  before any system call.

All `.InvalidInput` results are produced before any resource is created.

#### Listening and connecting

- `tcpListen` creates a stream socket, binds it, and starts listening. On POSIX
  it sets address reuse first so that a restarted server can bind while
  closed connections remain in `TIME_WAIT`. On Windows it does not set
  `SO_REUSEADDR`, because there it permits another process to bind the same
  port; the default exclusive behavior is kept. A port in use returns
  `.AddressInUse`. A failure after the socket exists closes it before returning.
- `tcpConnect` creates a stream socket and connects to the address, blocking
  until the connection is established or fails. A closed port is
  `.ConnectionRefused`. The local address and port are chosen by the system.
  It exists chiefly so that programs and tests can speak to their own
  listener; it is a complete client for literal addresses.
- `tcpLocalPort` returns the port a listener is bound to.
- Socket and readiness queue handles are non-inheritable before they are
  returned, so a child started by `runProcess` does not share them with its
  parent. See [§21.6](21-concurrency.md#216-restrictions) for the restriction
  on running processes alongside tasks.

#### Accepting

`tcpAccept` blocks until a connection is complete and returns it, or until its
timeout expires (see Timeouts). An interrupted wait is retried internally. Every
other failure is returned; the program decides whether to continue accepting. The
peer's address is not reported in this version.

#### Timeouts

[Decision 0014](../docs/decisions/0014-network-timeouts.md). `tcpAccept` and
`tcpReceive` take `timeoutMillis`:

- `-1` waits without limit, as these operations did before this parameter existed.
- `0` does not wait: the call succeeds only if it can complete now.
- `1` to `2147483647` waits at most that many milliseconds for readiness, once
  per call. A listener is ready when a connection can be accepted. A connection is
  ready when `tcpReceive` can return without blocking, which includes data, end
  of stream, and an error; the call then returns the data, the empty string, or
  the error exactly as it would have after an unlimited wait.
- When the wait expires the call returns `.Err(.TimedOut(code))`, with `code`
  `WSAETIMEDOUT` (10060) on Windows or `ETIMEDOUT` (Linux 110, macOS 60).
  No data is consumed and the handle stays valid.
- A wait interrupted by a signal restarts with its full timeout.

The receive and accept limit bounds one wait, not a sequence of calls. Callers
can track a longer-lived deadline with the monotonic clock (§18.8).

`tcpConnectWithin` and `tcpSendWithin` use the same timeout range, checked
before opening a socket or touching a connection, even for an empty send.
`-1` uses the unbounded operation. `0` permits immediate nonblocking progress,
but no wait. A positive timeout shares one monotonic budget across waits and
partial progress, rather than restarting after a short write or interrupted
wait. Expiry returns `.TimedOut`; scheduling and host timer resolution can
delay the return. A clock query failure returns `.Other(code)`.

`tcpConnectWithin` checks the socket error after a pending connect becomes
ready, and publishes a blocking connection only on success. If the host reports
readiness within the bounded wait, that result wins even if rescheduling is late.
Failure closes the
new socket. `tcpSendWithin(connection: &connection, ...)` temporarily uses
nonblocking mode. A send error, including expiry, may follow acceptance of an
unknown prefix: do not retry the entire message blindly. If restoring blocking
mode fails, that error takes precedence; the function invalidates and closes
the owner. Later I/O returns `.InvalidInput`; explicit close and destruction
have nothing left to close.

#### Polling several sockets

`tcpPoll` borrows both arrays and waits on the whole set. It accepts 1 to 64 sockets
combined, with either array allowed to be empty. The timeout uses the range above.
An invalid count or timeout returns `.InvalidInput` before calling the OS.

`TcpReadiness` has public `connections` and `listeners` fields of type `[Int]`.
Each contains zero-based indices into its corresponding input array, in ascending
input order. Timeout is success with two empty lists. A pending connection, readable
data, end of stream, or a socket error may indicate readiness; an invalid local
descriptor or an interrupted poll is an error. Polling does not read, accept, or
clear socket errors. Readiness is advisory: a subsequent blocking call is not
guaranteed to complete immediately.

The bounded implementation uses `WSAPoll` on Windows, `ppoll` on Linux, and
`poll` on macOS.

#### TCP read readiness queues

> Provisional; see [decision 0032](../docs/decisions/0032-tcp-read-queue.md).

`tcpOpenReadQueue` consumes an array of 1–4096 live local connection owners.
An invalid count or a locally invalidated owner returns `.InvalidInput`
before registering sockets. On any setup failure, the call closes all inputs
before returning the error. Native resource limits may prevent a queue from
reaching the API ceiling. Ownership is transferred to the call on every
outcome.

`tcpReadQueueWait` takes the same timeout range as `tcpPoll`. An invalid timeout
does not change the current selection. A valid wait releases that selection,
rearming the socket unless a read already confirmed EOF or a terminal error;
in those cases it closes and retires the slot instead, without reporting
close errors. The wait returns
`.Ok(.Some(index))` when it selects a connection and `.Ok(nil)` on expiry or
when no connections remain, even with an unlimited timeout. A timeout leaves
no selection and does not cancel registrations. A positive timeout shares a
single monotonic budget across rearm, interruptions and the wait; zero does
not block for readiness. Indices are zero-based and stable for the lifetime
of the queue. Notification order and fairness are unspecified.

Readiness is advisory: data, EOF or a socket failure can make a slot ready.
It does not itself consume data or confirm EOF. `tcpReadQueueReceive` makes a
nonblocking read of up to `maximumBytes` (1–1048576). `.Ok(nil)` means a read
would block, `.Ok(.Some(""))` confirms EOF, and a nonempty string holds the
bytes read. A failed read probe selects its slot, with the socket error
available to receive if there are no bytes to return. Confirmed EOF and
terminal read errors repeat until the selection is released or closed.

`tcpReadQueueSendWithin` sends on the selected socket with a timeout of
0–2147483647 milliseconds and the same partial-send rule as `tcpSendWithin`.
It can be used to reply after a read confirms EOF, before releasing the slot.
`tcpReadQueueCloseCurrent` clears the selection and permanently retires the
slot; a reported close error does not revive it. These three operations
return `.InvalidInput` without a selection. Bad sizes or budgets leave the
selection unchanged.

An unrecoverable queue-level wait or rearm failure closes all owned sockets
and retains that failure for subsequent calls with valid arguments.
Destroying a queue closes all remaining connections and releases its
registrations. A queue can move between tasks, but all use is exclusive.
There is no operation to extract a connection, insert a new connection, or
register a listener. Waiting does not scan the full set.

#### Receiving

`tcpReceive` blocks until at least one byte arrives, the peer finishes sending,
an error occurs, or its timeout expires.

- Success with `1` to `maximumBytes` bytes is a *short read*: the result is
  whatever the system returned, never padded or waited for, and the contents
  are arbitrary bytes (embedded NUL and invalid UTF-8 allowed).
- Success with an empty `String` means an orderly end of stream: the peer has
  closed or shut down its sending side. Every later call returns the empty
  string again. It is not an error.
- An abortive close or network failure returns `.ConnectionReset` or
  `.TimedOut` as classified below.
- The runtime never holds more than `maximumBytes` bytes for one call,
  whatever the peer sends.

Nothing frames messages: a request can arrive in many pieces, and several
requests can arrive in one. Protocol code (HTTP, lines) is the caller's.

#### Sending

`tcpSend` writes all of `contents`, repeating partial writes. Success means the
bytes were accepted by the local system, not that the peer read them. An empty
string succeeds without a system call. On failure part of `contents` may
already have been sent; no rollback is promised. A send to a peer that has
closed returns `.ConnectionReset` and never terminates the process with
`SIGPIPE` (`MSG_NOSIGNAL` on Linux, `SO_NOSIGPIPE` on macOS; Windows raises no
such signal).

#### Closing

`tcpCloseConnection` and `tcpCloseListener` take their handle by `consuming`:
the argument is dead after the call, whatever the result, and using it again is
the existing use-after-consume error. A close that reports failure still
releases the handle; closing again is impossible and not needed.

A normal close of a connection lets bytes already accepted for sending be
delivered before the connection ends; it neither waits for the peer nor
discards them. A handle that is destroyed without an explicit close, such as one that
goes out of scope or is dropped on an error path, is closed by its `deinit`, and a
failure to close is not reported. Close explicitly when the result matters.

#### Errors

`NetworkError` payloads are `Int`. The payload is the original platform code:
`WSAGetLastError()` or `GetLastError()` on Windows, the positive `errno` on POSIX
(raw system calls on Linux return `-errno`, which is negated). `.InvalidInput` carries
the platform's invalid-argument code. Classification:

| Case | POSIX | Windows |
|---|---|---|
| `InvalidInput` | `EINVAL` | `WSAEINVAL`, `WSAEFAULT` |
| `AddressInUse` | `EADDRINUSE` | `WSAEADDRINUSE` |
| `AddressNotAvailable` | `EADDRNOTAVAIL` | `WSAEADDRNOTAVAIL` |
| `PermissionDenied` | `EACCES`, `EPERM` | `WSAEACCES` |
| `ConnectionRefused` | `ECONNREFUSED` | `WSAECONNREFUSED` |
| `ConnectionReset` | `ECONNRESET`, `EPIPE`, `ECONNABORTED` | `WSAECONNRESET`, `WSAECONNABORTED`, `WSAESHUTDOWN`; `ERROR_NETNAME_DELETED`, `ERROR_CONNECTION_ABORTED` for read-probe completions |
| `TimedOut` | `ETIMEDOUT` | `WSAETIMEDOUT` |
| `Other` | everything else, including a failed Winsock startup | everything else |

`NetworkError` is a separate type from `IOError`, `FileSystemError`,
`ProcessError`, `StderrError`, and `StdinError`. Postfix `?` propagates only
the same error type; adapters match and wrap explicitly.

#### Platform behavior

On Windows the first network operation starts Winsock (version 2.2) once for
the process and never cleans it up; a startup failure is `.Other(code)`.
Windows programs that use any of these operations link `ws2_32`. Program text
does not name a library ([§19.7](19-c-interop.md)); the toolchain adds it.

On Linux the operations are system calls made directly (decision 0011), with no
C library or resolver; a program that uses them needs no library beyond the C
startup that every Linux program already has.

On macOS the operations use libSystem. The read queue registers socket read
events in `kqueue`; a hangup can be reported with bytes still buffered.

#### Not provided

These calls are synchronous on the calling thread. A slow client can still
stall unbounded `tcpSend`. There is no exposed socket mode, general cancellation,
or dynamic readiness set. There is no one-directional
`shutdown`, no peer or local address query beyond `tcpLocalPort`, no socket
options, no host-name resolution, no IPv6, no UDP, no Unix-domain sockets, and
no TLS. Binding `"0.0.0.0"` exposes the program to the network; these
operations are not a sandbox and add no authentication or rate limiting.

### 18.7 Process environment and current directory

```joyeer
public enum HostQueryError {
    InvalidInput(Int),
    InvalidEncoding(Int),
    QueryFailed(Int),
    OutOfRange(Int),
}

public func environmentVariable(name: String): Result<String?, HostQueryError>
public func currentDirectory(): Result<String, HostQueryError>
```

Lookups do not mutate process state. An environment name is nonempty, valid
UTF-8, and contains neither NUL nor `=`. Lookup follows native case rules
(byte-exact on POSIX and case-insensitive on Windows). `.Ok(nil)` means absent;
an existing empty value is `.Ok(.Some(""))`. Returned strings are owned and
must be valid UTF-8. On Windows, unpaired UTF-16 surrogates cause an encoding
error, not replacement. Concurrent foreign environment mutation is outside
the contract; Linux looks up the environment inherited at startup.

`currentDirectory` returns the process's absolute host path, not the `PWD`
environment variable. It can fail if the current directory cannot be named.
Results are not guaranteed stable across calls, and they neither resolve
arbitrary paths nor establish filesystem confinement.

The error cases preserve native codes: `InvalidInput` uses `EINVAL` or
`ERROR_INVALID_PARAMETER`, `InvalidEncoding` uses `EILSEQ` or
`ERROR_NO_UNICODE_TRANSLATION`, and `QueryFailed` carries the original query
failure code. `OutOfRange` uses `EOVERFLOW` or `ERROR_ARITHMETIC_OVERFLOW`.

### 18.8 Clocks

```joyeer
public func monotonicMilliseconds(): Result<Int, HostQueryError>
public func wallClockMilliseconds(): Result<Int, HostQueryError>
```

The monotonic clock has an unspecified origin. Nonoverlapping calls in one
process return nondecreasing readings, which can repeat at host resolution.
Use differences for elapsed time; the readings are not absolute timestamps.
Whether time during system suspend is included is unspecified.

Wall time is signed milliseconds since the Unix epoch, rounded down, and can
move backward. Neither clock promises millisecond precision. Native query
failures and unrepresentable readings are reported rather than wrapped.

### 18.9 Owned sequential files and directories

```joyeer
public func openFileRead(path: String): Result<FileReader, FileSystemError>
public func openFileNew(path: String): Result<FileWriter, FileSystemError>
public func readFileChunk(file: inout FileReader, maximumBytes: Int): Result<String, FileSystemError>
public func writeFileChunk(file: inout FileWriter, contents: String): Result<Void, FileSystemError>
public func closeFileReader(file: consuming FileReader): Result<Void, FileSystemError>
public func closeFileWriter(file: consuming FileWriter): Result<Void, FileSystemError>

public func openDirectory(path: String): Result<DirectoryReader, FileSystemError>
public func nextDirectoryEntry(directory: inout DirectoryReader): Result<String?, FileSystemError>
public func closeDirectory(directory: consuming DirectoryReader): Result<Void, FileSystemError>
```

These types and `ChildProcess` are noncopyable owners with no public native
token or constructor. Native file and directory handles are non-inheritable.
Normal destruction releases the resource exactly once; close errors are
ignored in `deinit`. Use explicit consuming close to report release and flush
errors. On error the caller still loses ownership; close is not retried on a
possibly recycled token.

The file operations follow §18.2's path encoding, byte preservation, and
exclusive-create rules. A reader starts at offset zero. Each read requests
from 1 through 65,536 bytes; an invalid size reports `.Other` with the native
invalid-argument code and does not advance the cursor. A read may return fewer
bytes than requested. An empty successful chunk indicates EOF, which is sticky.
A directory may fail either at open or at the first positive read. Writes
append sequentially, completing the entire chunk on success; data can be
buffered until close. An error may follow partial progress, and there is no
rollback or durability promise. After an I/O failure, further I/O reports
the same error; the owner remains closeable.

Directory iteration returns names as in `listDirectory`, one at a time in
unspecified order. `.Ok(nil)` means end, stays at end, and is distinct from
invalid encoding or other errors. Errors are sticky until close. An empty
iterator may own no native token. Neither iteration nor any file open is
a confinement check. This interface does not provide seeking or asynchronous I/O.

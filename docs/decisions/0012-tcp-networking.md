# 0012: Blocking TCP networking in `System`

- Status: proposed (2026-11-08). Nothing is implemented. The first consumer is
  the [markdown web server example](../../example/markdown-server/README.md),
  which is written against this API and cannot be built until it lands.
- Spec: [§18.6 TCP networking](../spec/18-host.md#186-tcp-networking)
  (provisional)

## Context

Joyeer's stated purpose is to write modern replacements for legacy software.
Much of that software is a network service, and today a Joyeer program cannot
open a socket: `System` has files, one synchronous child process, and the three
standard streams. The smallest useful network program is a static web server,
for example one that serves a directory of Markdown files as a site.

Constraints:

- No garbage collector and deterministic destruction, but no user-defined
  `deinit` yet, so a handle cannot close itself.
- The language has no threads, closures, `async`, or generics. A first API must
  not depend on any of them and must not foreclose them.
- Windows `System` calls only `kernel32` and Linux `System` makes raw system
  calls ([0011](0011-runtime-without-the-c-library.md)). Winsock lives in
  `ws2_32`, which the Windows link does not name today.
- The FFI is restricted ([§19](../spec/19-c-interop.md)): scalars, bare
  `[UInt8]` data pointers, no C structures, no callbacks, no variadic calls.
  Socket structures are built and read as byte buffers at fixed offsets, as
  `stat` and `dirent` already are ([0003](0003-pure-joyeer-filesystem.md)).
- "We will go deep later." The first version should be small, blocking, and
  honest about what it lacks.

## Options considered

1. **A bound HTTP server in the runtime** (`serve(port:, handler:)`). Needs
   closures or function values, which the language does not have, and puts
   HTTP policy in the platform layer. Rejected.
2. **Blocking TCP sockets in `System`, IPv4 literals only.** A listener and a
   connection are opaque handles; calls are `listen`, `accept`, `connect`,
   `receive`, `send`, `close`. HTTP, TLS and routing are libraries written in
   Joyeer on top. Matches how `System` already exposes the filesystem and
   processes, and needs no new language feature.
3. **Full sockets first** (UDP, IPv6, DNS, non-blocking, timeouts, `poll`,
   socket options, peer addresses). The right destination, but most of it needs
   decisions the language has not made yet (concurrency, time, `Duration`),
   and every item is more native code to verify on three operating systems.
   Deferred, not rejected.
4. **Bind libc/Winsock `getaddrinfo` for names.** Brings in a C library
   dependency that decision 0011 removed on Windows and Linux, and a C
   structure the FFI cannot express. Deferred.

## Decision

Option 2, as [§18.6](../spec/18-host.md#186-tcp-networking):

- `System` gains `TcpListener`, `TcpConnection`, `NetworkError`, and eight
  functions: `tcpListen`, `tcpAccept`, `tcpConnect`, `tcpLocalPort`,
  `tcpReceive`, `tcpSend`, `tcpCloseConnection`, `tcpCloseListener`.
- Every operation blocks. There are no timeouts, no non-blocking mode, and no
  readiness polling. One blocked call blocks the whole program.
- Addresses are dotted-quad IPv4 literals parsed in Joyeer. No names, no IPv6.
- A handle is released only by an explicit `consuming` close. The type system
  then rejects use after close; the leak case (never closing) is a
  documented limitation until `deinit` exists.
- `tcpReceive` returns an owned `String` of 1 to `maximumBytes` bytes, or an
  empty string at orderly end of stream. The bound is mandatory, like
  `readStdinLine` ([0001](0001-stdin-line-reading.md)), so every read has a
  memory ceiling whatever the peer sends.
- Errors carry the original platform code (`WSAGetLastError` on Windows, the
  positive `errno` elsewhere) in an `Int` payload, like the other host errors.
- Handles are never inherited by `runProcess` children, and a send to a closed
  peer returns an error instead of raising `SIGPIPE`.

Implementation shape (informative, to be confirmed while building):

- Windows: `ws2_32` through `@c_extern`, `WSAStartup(2.2)` once on first use,
  `WSASocketW` with no-inherit. The link line gains `ws2_32.lib` next to
  `kernel32.lib`; import libraries only pull in referenced symbols, so programs
  that do not use sockets are unchanged. Winsock function names are lower case,
  so the own-entry classifier in `linker.cpp` will treat a program that uses
  them as calling the C library and keep the CRT startup; correct, merely
  heavier, and an allowlist can remove that later.
- Linux: raw system calls (`socket`, `bind`, `listen`, `accept4`, `connect`,
  `recvfrom`, `sendto`, `shutdown`, `getsockname`, `setsockopt`, `close`)
  through `joyeer_intrinsic_syscall`, selected by architecture as in 0011.
- macOS: libSystem. `sockaddr_in` carries a length byte there; `SO_NOSIGPIPE`
  replaces `MSG_NOSIGNAL`.
- New files `lib/System/network.joyeer` (portable logic: address parsing,
  argument validation, loops, error classification) and
  `network_windows.joyeer`, `network_posix.joyeer`, `network_linux.joyeer`
  (primitives), registered in `lib/System/project.toml`.

## Consequences

- Easier: a static file server, an echo or line protocol server, and a client
  for tests are all writable in Joyeer. HTTP and Markdown rendering stay
  library code and can evolve without touching the platform layer.
- Harder: the API is single-connection-at-a-time. A slow or silent client
  stalls the server. The example documents this and recommends a reverse proxy
  for anything exposed to untrusted networks. Concurrency is the first item of
  the next round and is a language decision, not a library one.
- Harder: no TLS. Anything exposed beyond loopback should sit behind a TLS
  terminating proxy.
- Tests: a loopback round trip needs no threads, because the kernel completes
  the TCP handshake into the listen backlog: `tcpListen` on port 0,
  `tcpLocalPort`, `tcpConnect`, `tcpAccept`, `tcpSend`/`tcpReceive` both ways,
  close, then receive returns the empty string. Also: invalid address and port
  forms, `AddressInUse` on a second listen, `ConnectionRefused` on a closed
  port, the receive bound, a large send that needs several partial writes, and
  no descriptor inheritance across `runProcess`. Windows tests check that the
  import set grows by `ws2_32` only for programs that use sockets.
- Diagnostics: none new at compile time. The new names are ordinary `System`
  functions; misuse after close is the existing use-after-consume diagnostic.
- Open questions to settle during implementation:
  1. Whether `System` can make the handle field of `TcpListener` /
     `TcpConnection` inaccessible to importers (file-local `private` applies to
     declarations; field-level access is not documented). If not, the types are
     opaque by contract only.
  2. How strong the `consuming` close really is. Compiling the example against
     a stub of this API showed that a `match` pattern binding such as
     `.Ok(connection)` is not an owning local, so
     `tcpCloseConnection(connection: consume connection)` is rejected
     (`type-checking.invalid-consume-argument`). Postfix `?` does produce an
     owning local, so the example accepts through `let connection =
     tcpAccept(listener: listener)?`. Callers that must `match` first would have
     to copy the handle into a local, creating a second owner of the same
     resource. Whether to leave this as is, let a pattern binding be consumed
     when the scrutinee is a temporary, or make the close take a borrowed
     handle is open.
  3. Whether the macOS and Linux AArch64 variants should be gated behind the
     same acceptance steps as the POSIX filesystem, which has not run there.

## Evidence

- Decision 0001 and 0006: the repository's precedent is small, bounded,
  blocking host operations with explicit error payloads, added when a real
  consumer needs them.
- The web server example is that consumer. Its design is deliberately the
  least a Markdown site needs: accept, read one request head, send one
  response, close.

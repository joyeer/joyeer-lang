# 0014: Receive and accept timeouts

- Status: proposed (2026-11-08). First step (1a) of stage 1 in the
  [concurrency plan](../plan/concurrency.md). Implemented for Windows and Linux
  in the same change and verified on Windows x64 and Linux x86-64 (WSL2) Debug:
  `SystemTests` (the timeout cases) and the web server over real connections,
  where a silent client received `408` after 5.0 s and later clients were served.
  The Linux AArch64 `ppoll` number and macOS are untested; full validation and
  the role reviews have not run.
- Spec: [§18.6 TCP networking](../spec/18-host.md#186-tcp-networking)
  (provisional)

## Context

Every operation in [0012](0012-tcp-networking.md) blocks, so a client that
connects and sends nothing stalls the whole server, which serves one connection
at a time. [0013](0013-concurrency-model.md) fixes the long-term answer
(share-nothing threads), but a timeout is the smallest change that makes a
single-threaded server survive a silent client, and it stays useful with threads.

Constraints:

- Sockets are blocking, and there is no non-blocking mode in the API. Nothing in
  `System` exposes a clock, so a program cannot compute a total deadline from
  several waits.
- Each platform can wait for readiness without changing a socket's mode:
  `WSAPoll` on Windows and the `ppoll` system call on Linux (no libc, 0011).
- `SO_RCVTIMEO` is not a uniform answer: Winsock ignores it for `accept`, it
  leaves `connect` and a large `send` unbounded, and it is hidden per-handle state.
- The API is provisional, so its signatures can still change.

## Options considered

1. **A `timeoutMillis` argument on the waiting calls.** Explicit at every call
   site, no hidden state, and a reviewer sees which waits are bounded. Costs a
   signature change and an argument at every blocking call.
2. **A per-handle setting** (`tcpSetTimeout(handle, ...)`), implemented with
   `SO_RCVTIMEO`. Less churn, but the setting is invisible at the call, it needs a
   field on a handle that is passed by borrow, and Winsock does not apply it to
   `accept`. Rejected.
3. **`...Timeout` function variants beside the blocking ones.** Doubles the
   surface for two functions. Rejected.
4. **Non-blocking sockets plus a readiness set** (`tcpPoll`, stage 1b). Needed
   eventually for `send` and `connect` timeouts, but a larger design; deferred.

## Decision

Option 1, for the two waits that a silent peer controls:

```joyeer
func tcpAccept(listener: TcpListener, timeoutMillis: Int): Result<TcpConnection, NetworkError>
func tcpReceive(connection: TcpConnection, maximumBytes: Int, timeoutMillis: Int): Result<String, NetworkError>
```

- `timeoutMillis` is `-1` (wait without limit, the previous behavior), `0` (do
  not wait: succeed only if the operation is ready now), or `1` to `2147483647`
  milliseconds. Any other value is `.Err(.InvalidInput(code))` before any system
  call.
- The limit applies to the wait for readiness, once per call. A listener is ready
  when a connection can be accepted; a connection is ready when `tcpReceive` can
  return without blocking, which includes data, end of stream, and an error (the
  call then reports the error or the end of stream as it would have).
- When the wait expires the call returns `.Err(.TimedOut(code))`. `code` is
  `WSAETIMEDOUT` (10060) on Windows and `ETIMEDOUT` (110) on Linux, a code the
  platform defines even though the wait itself reports no error. No data is
  consumed and the handle stays valid and usable.
- A wait interrupted by a signal is restarted with its full timeout, so a very
  frequent signal could extend it. Joyeer installs no signal handlers.
- A single wait is bounded; a loop that calls `tcpReceive` many times is not. An
  attacker who sends one byte just before each timeout still holds a connection
  open, bounded by the caller's own cap on bytes read. A total deadline needs a
  monotonic clock, which `System` does not yet have; it is a separate, later
  addition.

Not decided here: `tcpSend` and `tcpConnect` timeouts (they need non-blocking
sockets, because a blocking `send` of a large buffer waits after readiness) and
the readiness set (1b).

## Consequences

- Easier: a single-threaded server drops a silent client after its idle timeout
  and keeps serving the others. The web server example does this with a
  per-receive timeout and its existing 16 KiB cap on a request head.
- Harder: every call site writes a timeout argument, and `-1` is how a program
  says it accepts blocking forever. All earlier calls in tests and the example
  change.
- Tests (Windows and Linux, loopback, one thread): a receive with no data times
  out and the connection still works afterward; data present before the wait
  returns the data; `0` with no data times out; a closed peer returns the empty
  string, not a timeout; accept with no pending connection times out and with one
  pending returns it; out-of-range timeouts are rejected without waiting.
- No new compile-time diagnostics.

## Evidence

- 0012's "Not provided" and its consequence that one idle client holds every
  other client out.
- The concurrency plan lists 1a first because it needs no language change and
  helps every later design.

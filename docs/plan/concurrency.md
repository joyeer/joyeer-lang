# Concurrency: what is left

The concurrency work is done: network timeouts (P1a), noncopyable and linear types with `deinit` (P2),
thread primitives (P3), a thread-safe runtime (P4), tasks (P5a), channels (P5b, P6) and the worker-pool
web server (P7). The rules are in [spec §21](../../specs/21-concurrency.md), the decisions in
[0013](../decisions/0013-concurrency-model.md) and [0015](../decisions/0015-noncopyable-and-linear-types.md)
to [0019](../decisions/0019-channels.md), and the implementation notes in
[runtime](../impl/runtime.md) and [supported features](../impl/supported-features.md). This file keeps
only what is still open.

## Acceptance gates not yet run

- `scripts/validate.py` ran on Linux x86-64 (WSL2, Debug and Release): ctest 1476 of 1476 in both.
  It did **not** run on Windows (there is no Python there); the Windows Debug ctest suite was run by
  hand (1504 of 1504 on the last tree). Run it on a machine with Python 3.9 or later.
- **Windows Release**, **AArch64 Linux** and **macOS** (the thread, channel and `os_unfair_lock` paths
  are written but untested there).
- `fern test` reports `SystemTests` failing the "write zero bytes to a closed stderr" case on Linux
  under WSL2. It fails identically before the runtime-thread-safety work, so it is not a regression of
  this plan, but its cause is not understood.
- `-lpthread` is emitted for a program that starts a task or names a channel end type, but that link
  argument is untested on glibc older than 2.34 (the local glibc folds pthread into libc).
- The Windows C-startup link (`/WHOLEARCHIVE`) and macOS (`-force_load`) load every runtime archive
  member, so those programs import the thread and channel functions even when unused. Fix with a plain
  archive plus `/INCLUDE:wmain` on Windows, with a regression test.

## Language and compiler gaps found along the way

- A `match` that binds the payload of an owned temporary (`receiver.receive()`, `removeLast()`) **clones**
  a copyable payload (`String`, array) instead of moving it. Moving it for every type would make
  `receive` zero-copy in the common case.
- Linear moves into array literals, dictionary literals and enum payloads are not tracked as pending
  until the construction commits (calls and initializers are); a `?` in a later element can still drop a
  handle (the run-time backstop traps `task dropped without join`).
- A qualified builtin enum case, `ChannelClosed.Closed` (and `ConversionError.OutOfRange`), fails in IR
  lowering; `consume Chain.End` (a qualified payload-less case) is rejected; `==` on a struct or enum is
  a diagnostic with no workaround; a function or method used as a value, and `spawn(...).join()`, report
  late or misleading diagnostics.
- A joined result cannot be an argument (`print(value: consume h.join())`) because the argument marker
  and the receiver marker collide; bind it first.
- A channel that carries its own receiver leaks (the only reference is in the queue).
- Element types `Any` and `Never` for channels are checked but cannot be written (the lexer rejects the
  names).
- Dispatch to a worker is round-robin; one request in N waits for a stalled worker's read timeout
  (measured in the [example README](../../example/web-server/README.md)). Fixing it needs either
  multi-sender channels, a `select` or a try-receive, so the accept loop can learn which worker is idle
  (decision 0013, question 11).
- Possible warnings: a `join` while a channel end of the same function is still live (the close-before-join
  deadlock), and a discarded `Result` of `send` or `receive`.

## Network work not done

- Send and connect timeouts (they need non-blocking sockets) and a total deadline for a request head.
- `tcpPoll` over several handles with a hard cap (`WSAPoll`, `ppoll`), and a readiness API that scales
  past a poll set (`epoll`, IOCP, `kqueue`).

## Deferred, not forgotten

- Multi-sender channels, which need a runtime sender count and would be the one named exception to §4.1
  and §4.8; unbounded channels, `trySend` (returning the value, with a `SendError<T>`), `tryReceive`,
  timeouts on channel operations, and iterating a receiver.
- Structured scopes that lend read-only borrows to child tasks; `actor` syntax as sugar over a task and
  its channel; `async`/`await`, only if tens of thousands of simultaneous connections need it.
- Per-thread allocator caches with lock-free remote frees, and raw `clone`/`futex` threads if the C
  library is ever dropped on Linux.
- Lifting the `runProcess` ban in programs that start tasks (decision 0018, open question 1): the Linux
  child no longer allocates before `execve`, and the ban stops Fern from running its executor in
  parallel.
- An IR `linear` bit with no scope-end cleanup for linear values, a verifier rule and a wire bump, as
  defence in depth beyond the run-time trap.

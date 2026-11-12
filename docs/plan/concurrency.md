# Concurrency: Plan

- Status: in progress (updated 2026-11-12); the feature work is done, gates and gaps remain
- Decisions: [0013](../decisions/0013-concurrency-model.md), [0014](../decisions/0014-network-timeouts.md),
  [0015](../decisions/0015-noncopyable-and-linear-types.md) to [0019](../decisions/0019-channels.md)
- Spec: [§21](../../specs/21-concurrency.md)

## Goal

Finish share-nothing concurrency: run the acceptance gates that have not run, close the language and
compiler gaps the feature work found, and keep the deferred features on record.

## Definition of done

- `scripts/validate.py` passes on every supported platform, including Windows Release, AArch64 Linux
  and macOS.
- Every item under "Language and compiler gaps" is fixed or moved to a decision record.

## Current state

Network timeouts (P1a), noncopyable and linear types with `deinit` (P2), thread primitives (P3), a
thread-safe runtime (P4), tasks (P5a), channels (P5b, P6) and the worker-pool web server (P7) are done.
The implementation notes are in [runtime](../impl/runtime.md) and
[supported features](../impl/supported-features.md).

`scripts/validate.py` ran on Linux x86-64 (WSL2, Debug and Release): ctest 1476 of 1476 in both. On
Windows the Debug ctest suite was run by hand (1504 of 1504 on the last tree), because there is no
Python there.

## Todo

### Acceptance gates

- [ ] Run `scripts/validate.py` on Windows, on a machine with Python 3.9 or later.
- [ ] Windows Release.
- [ ] AArch64 Linux (the thread and channel paths are written but untested there).
- [ ] macOS, including the `os_unfair_lock` path.
- [ ] Find the cause of the `SystemTests` "write zero bytes to a closed stderr" failure under `fern test`
  on Linux WSL2. It fails identically before the runtime-thread-safety work, so it is not a regression.
- [ ] Test the `-lpthread` link argument (emitted for a program that starts a task or names a channel
  end type) on glibc older than 2.34.
- [ ] Stop the Windows C-startup link (`/WHOLEARCHIVE`) and macOS (`-force_load`) from loading every
  runtime archive member: a plain archive plus `/INCLUDE:wmain` on Windows, with a regression test.

### Language and compiler gaps

- [ ] A `match` binding the payload of an owned temporary (`receiver.receive()`, `removeLast()`) moves
  instead of cloning a copyable payload (`String`, array), making `receive` zero-copy.
- [ ] Track linear moves into array literals, dictionary literals and enum payloads as pending until
  construction commits, so a `?` in a later element cannot drop a handle (today the run-time backstop
  traps `task dropped without join`).
- [ ] Qualified builtin enum cases (`ChannelClosed.Closed`, `ConversionError.OutOfRange`) lower in IR.
- [ ] `consume Chain.End` (a qualified payload-less case) is accepted.
- [ ] `==` on a struct or enum, or a diagnostic with a workaround.
- [ ] Early, accurate diagnostics for a function or method used as a value, and for `spawn(...).join()`
  (see the [generics plan](generics.md), Phase 1).
- [ ] A joined result can be an argument (`print(value: consume h.join())`); today the argument marker
  and the receiver marker collide.
- [ ] A channel that carries its own receiver does not leak.
- [ ] Channel element types `Any` and `Never` can be written (the lexer rejects the names), or are
  rejected by the type checker.
- [ ] Dispatch to the idle worker instead of round-robin; one request in N waits for a stalled worker's
  read timeout ([example README](../../example/web-server/README.md)). Needs multi-sender channels, a
  `select` or a try-receive (decision 0013, question 11).
- [ ] Decide on warnings for a `join` while a channel end of the same function is live (the
  close-before-join deadlock) and for a discarded `Result` of `send` or `receive`.

### Networking

- [ ] Send and connect timeouts (they need non-blocking sockets) and a total deadline for a request head.
- [ ] `tcpPoll` over several handles with a hard cap (`WSAPoll`, `ppoll`).
- [ ] A readiness API that scales past a poll set (`epoll`, IOCP, `kqueue`).

## Open questions

- Lifting the `runProcess` ban in programs that start tasks (decision 0018, open question 1): the Linux
  child no longer allocates before `execve`, and the ban stops Fern from running its executor in
  parallel.

## Out of scope

Deferred, not forgotten:

- Multi-sender channels, which need a runtime sender count and would be the one named exception to §4.1
  and §4.8; unbounded channels, `trySend` (returning the value, with a `SendError<T>`), `tryReceive`,
  timeouts on channel operations, and iterating a receiver.
- Structured scopes that lend read-only borrows to child tasks; `actor` syntax as sugar over a task and
  its channel; `async`/`await`, only if tens of thousands of simultaneous connections need it.
- Per-thread allocator caches with lock-free remote frees, and raw `clone`/`futex` threads if the C
  library is ever dropped on Linux.
- An IR `linear` bit with no scope-end cleanup for linear values, a verifier rule and a wire bump, as
  defence in depth beyond the run-time trap.

## Progress log

- 2026-11-12: converted to the plan template; no items changed.

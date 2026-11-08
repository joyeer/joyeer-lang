# Concurrency: Plan and Todo List

> **Status:** proposed. Implements [decision 0013](../decisions/0013-concurrency-model.md)
> (share-nothing threads, ownership moved through tasks and bounded channels) after
> the readiness step it names as stage 1. Nothing here is implemented. Tick items as
> they land and delete the plan when the work is done. This plan adds no language
> rule; the rules arrive with the spec chapter in phase 7.

**Why now.** The blocking network API ([0012](../decisions/0012-tcp-networking.md))
serves one connection at a time, so one silent client stalls every other. The
[web server example](../../example/web-server/README.md) is the first consumer and
the acceptance test for each phase.

**Sizes** are rough: S is days, M is a week or two, L is several weeks.

## Order and dependencies

```text
P0 decisions ─┬─ P1 timeouts + readiness (stage 1) ───────────────┐
              └─ P2 thread-safe runtime ─ P3 thread primitives ─ P4 compiler ─ P5 library ─ P6 server ─ P7 acceptance
```

P1 needs no language change and does not depend on P2 to P5, so it can ship
first and stand alone. P2 must not slow or change any single-threaded program.

## P0: Settle the design (S)

- [ ] Run the `language-designer` review on 0013 and record the findings.
- [ ] Resolve the open questions in 0013 that block P4: structured scope or
      detached tasks (Q1), function reference form for `spawn` (Q2), how a channel
      closes (Q4), bounded only or also unbounded (Q5).
- [ ] Decide what `join` returns for a task that traps (Q3: abort the process
      for now; keep the result type widenable).
- [ ] Decide whether `deinit` lands before stage 2 (Q6). If yes, it is its own
      feature with its own decision and this plan waits on it for task and channel
      cleanup; if no, tasks and channel ends are closed explicitly.
- [ ] Mark 0013 `accepted` or amend it. Write the new decisions listed below as
      each phase starts, not all up front.

## P1: Timeouts and readiness, single thread (M)

Decision to write: 0014, network timeouts and readiness. Spec: additions to
[§18.6](../spec/18-host.md#186-tcp-networking).

- [ ] Specify receive, send, accept and connect timeouts (milliseconds, `0`
      meaning poll, a documented "no timeout" value) and a new `TimedOut` use that
      is distinct from a connection reset.
- [ ] Specify a readiness wait over several handles, for example
      `tcpPoll(connections:, listener:, timeoutMillis:)` returning which are ready
      to read, with a hard cap on the set size and no unbounded allocation.
- [ ] Windows: implement over `WSAPoll` (or overlapped I/O if `WSAPoll`'s
      known limits matter); keep `ws2_32` the only new import.
- [ ] Linux: implement over the `ppoll` (or `poll`) system call, x86-64 and
      AArch64 numbers; no libc.
- [ ] macOS: still a placeholder returning `.Other(38)`; note it in the spec.
- [ ] Tests: timeout fires with no data; data arriving before the timeout wins;
      a ready subset is reported; a closed peer reads as ready (end of stream);
      zero timeout polls; invalid sets rejected. Loopback only, single thread.
- [ ] Rewrite `example/web-server` as an event loop: a bounded array of
      connection records, a per-connection idle deadline, and a cap on open
      connections. Keep the blocking version's request parsing and rendering.
- [ ] Acceptance: with a deliberately silent client connected, other clients are
      still served, and the silent one is dropped at its deadline, on Windows x64 and
      Linux x86-64 (WSL).
- [ ] Optional, Linux only: document running several server processes with
      `SO_REUSEPORT` as a scale-out path (needs a listen option; not required here).

## P2: Make the runtime thread-safe (M to L)

Decision to write: 0015, runtime thread safety. Every item must leave a
single-threaded program's behavior and the leak counter unchanged.

- [ ] Baseline: record the runtime benchmark and allocation-heavy fixtures before
      any change, so regressions are measurable.
- [ ] Audit every runtime global in `lib/Runtime/core/` (allocator, streams,
      entry, panic, static slots 0 to 63) and write down which are per-program,
      which per-thread, and which need a lock or atomic. Slot 16 (`envp`) and the
      argument vector are written once before any thread starts.
- [ ] Allocator: make free-list and region access safe under a lock first
      (a global lock built on the atomic and a blocking wait from P3), keeping the
      block layout (no header). Cross-thread frees are legal because the size
      arrives with `free`. Per-thread caches and lock-free remote frees are a later
      optimization, decided by a benchmark.
- [ ] Standard streams: guard the shared stdout buffer so one `print` is atomic;
      serialize standard input; keep flush-before-read and flush-before-panic.
- [ ] Panic and exit: define the sequence when a task traps (flush output,
      write the message once even if two tasks trap together, exit the process).
- [ ] Add a thread-local pointer intrinsic per target (`FS`/`GS` on x86-64,
      `TPIDR_EL0` on AArch64, the TEB on Windows) and a way for the thread creator
      to set it; define what per-thread state the runtime keeps (panic context first).
- [ ] Spec §19.4.5 gains any new runtime-only intrinsics; update the symbol gates.
- [ ] Tests: the existing runtime unit tests and the full suite pass unchanged;
      a stress test that allocates and frees from two OS threads started by the test
      harness (C or a test-only native helper) and checks the leak counter returns to
      zero.

## P3: Thread primitives per platform (L)

Decision to write: 0016, thread creation and blocking without a C library.

- [ ] Windows: `CreateThread`/`ExitThread`, `WaitOnAddress`/`WakeByAddress*`
      (or events), thread-pointer setup; only `kernel32`/`kernelbase` imports.
- [ ] Linux x86-64: `clone` with `CLONE_VM | CLONE_THREAD | CLONE_SIGHAND |
      CLONE_SETTLS | CLONE_CHILD_CLEARTID` on a runtime-mapped stack with a guard
      page, `futex` wait and wake, thread exit via `exit` (not `exit_group`) and
      join via the cleared-tid futex.
- [ ] Linux AArch64: the same with its numbers and `TPIDR_EL0`; untested until
      run natively, so the plan keeps it explicit (as 0011 does).
- [ ] macOS: libSystem `pthread_create`/`pthread_join` and a condition or
      `os_unfair_lock`; written but gated until run on a Mac.
- [ ] A small internal API in `lib/Runtime/` (create thread running a runtime
      function with one argument, join, a futex-style wait on an `Int` address, wake,
      yield) that the compiler-known types in P4 and P5 call. Keep it out of `System`.
- [ ] Decide stack size and whether a thread's stack is configurable.
- [ ] Tests: thread starts and returns; many short threads; join visibility
      (a write before exit is seen after join); wait/wake without lost wakeups;
      thread-pointer isolation; and the symbol-import gates still allow only the
      listed names on Linux and Windows.

## P4: Compiler support for tasks and channels (L)

Decision to write: 0017, task and channel types in the compiler. This is the
user-visible feature, so the [feature workflow](../decisions/README.md) applies:
spec, failing tests first, every pipeline stage, role reviews, validation.

- [ ] Spec chapter: tasks, channels, `spawn`, `join`, the `consuming` rules,
      the memory-model rules from 0013, and the new diagnostics.
- [ ] Reserved words: decide which of `async`, `await`, `actor` stay reserved
      (all three should) and whether `spawn` is a function or a keyword.
- [ ] Lexer/parser: whatever syntax `spawn` and the function reference need;
      syntax nodes carry spans.
- [ ] Name resolution: the reference to a named top-level function; no capture.
- [ ] Type checking: `Task<R>`, the sender and receiver ends, as compiler-known
      builtins like `Array`/`Result`; `consuming` required for the input; the
      deny-list of values that cannot cross a thread (native addresses); moved
      values are dead after `spawn` and `send`.
- [ ] Semantic analysis: use-after-move across `spawn`/`send`; and, if
      structured scopes are chosen, "task not joined" analysis.
- [ ] IR and verifier: operations for spawn, join, send, receive, close;
      ownership operations stay explicit; debug scopes preserved.
- [ ] Backend: lower to the P3 runtime calls; keep the C ABI unchanged.
- [ ] Fixtures per stage under `tests/` plus GoogleTests citing the spec
      sections, written failing first; e2e fixtures under `tests/e2e/`.
- [ ] Diagnostics with stable IDs for each rejection listed in 0013.

## P5: Library and runtime semantics (M)

- [ ] Runtime implementation of tasks: start, result slot, join, the trap path.
- [ ] Bounded channel: single ring buffer, send blocks when full, receive blocks
      when empty, close ends the receiver's loop, `clone` of a sender is a new owned
      end, last-sender-gone is detected.
- [ ] Moves versus copies: a `consuming` argument transfers heap storage with no
      clone (assert this with the allocation counter); an ordinary argument clones.
- [ ] Memory-order tests: a value sent is fully visible after receive; a result
      is visible after join; sequentially consistent edges as 0013 specifies.
- [ ] Stress tests: many tasks, many messages, cross-thread frees, closing during
      traffic, a trap in one task with others running; run repeatedly in CI, Debug
      and Release.
- [ ] Optional sanitizer or race-detector run of the runtime on Linux if a
      toolchain is available (no toolchain installation by automation).

## P6: Use it in the web server (S to M)

- [ ] Worker-pool version of `example/web-server`: the accept loop moves each
      `TcpConnection` through a channel to N worker tasks; workers close what they own.
- [ ] Combine with P1: workers still honor per-connection deadlines.
- [ ] Measure requests per second and tail latency against the single-thread
      event loop, and record it in the example README honestly, with the machine.
- [ ] Document the one-owner rule the example relies on, and that Markdown pages
      are read per request (no shared cache, by design).

## P7: Documentation and acceptance (M)

- [ ] Update the spec index, the preamble if goals change, `supported-features.md`,
      the portable skill's copy of it, and `docs/impl/runtime.md`.
- [ ] Run the role reviewers separately from the author: `language-designer`,
      `type-system-reviewer`, `ir-backend-reviewer`, and `adversarial-tester` on the
      new surface.
- [ ] `python3 scripts/validate.py --llvm-root <sdk>` on Windows x64 and Linux
      x86-64, Debug and Release; report unrun gates (AArch64, macOS) explicitly.
- [ ] Mark 0013 and its successors `accepted` and link the spec sections.
- [ ] Delete this plan, keeping anything still open in a smaller plan.

## Deferred, not forgotten

- Structured scopes that lend read-only borrows to child tasks.
- `actor` syntax as sugar over a task and its channel.
- `async`/`await`, only if tens of thousands of simultaneous connections need it.
- Per-thread allocator caches with lock-free remote frees.
- Unbounded channels, if a real consumer needs them.
- A readiness API that scales past a poll set (`epoll`, IOCP, `kqueue`).
- TLS (see the note in [0013](../decisions/0013-concurrency-model.md)); it
  needs the transport abstraction, not concurrency, and is its own decision.

## Risks

| Risk | Mitigation |
|---|---|
| Raw `clone`/`futex` threads and TLS without libc are easy to get subtly wrong | P3 is isolated and tested before any language feature depends on it; start on x86-64 Linux and Windows, where verification is possible here |
| The allocator lock slows single-threaded programs | Baseline in P2 before any change; take the lock only when a second thread exists |
| `spawn` forces a decision on closures | Keep it a compiler special form (0013 Q2) so the closure question stays open |
| No `deinit`, so handles and tasks can leak | Explicit close and join in v1; revisit when `deinit` exists |
| AArch64 and macOS cannot be verified on the usual machines | State them as untested in every record and spec section, as 0011 does |

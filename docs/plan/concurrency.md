# Concurrency: Plan and Todo List

> **Status:** proposed, revised after a `language-designer` review. Implements
> [decision 0013](../decisions/0013-concurrency-model.md) (share-nothing threads,
> ownership moved through tasks and bounded single-sender channels). Nothing here
> is implemented. Tick items as they land and delete the plan when the work is
> done. This plan adds no language rule: the spec chapter is the first item of the
> compiler phase (P5), written before any implementation, as the
> [feature workflow](../decisions/README.md) requires.

**Why now.** The blocking network API ([0012](../decisions/0012-tcp-networking.md))
serves one connection at a time, so one silent client stalls every other. The
[web server example](../../example/web-server/README.md) is the first consumer and
the acceptance test for the later phases.

**Sizes** are rough: S is days, M is a week or two, L is several weeks.

## Order and dependencies

```text
P0 design ─ P1a timeouts ─ (P1b readiness, optional)
          └ P2 language prerequisites ─┐
            P3 thread primitives + lock ─ P4 thread-safe runtime ─ P5 compiler ─ P6 runtime semantics ─ P7 server ─ P8 acceptance
```

- P1a needs no language change and ships first.
- P2 (noncopyable types and handles, owning a received value, array removal) has
  no dependency on threads and unblocks P5 and P1b.
- P3 builds the thread primitives and a minimal lock before P4 uses the lock;
  the first draft ordered these the other way around.
- P4 must not slow or change any single-threaded program.

## P0: Settle the design (S)

- [ ] Re-run the `language-designer` review on the revised 0013.
- [ ] Decide the 0013 open questions that block P5, using the recommendations
      in that record: task joining (Q1), the form of the function reference (Q2),
      closing (Q4), bounded only (Q5), single sender (Q7), the `receive` result (Q8),
      zero-parameter targets (Q9).
- [ ] Decide the questions that can wait but must be recorded: limits (Q10),
      dispatch `select` (Q11), output ordering (Q12), `runProcess` in a threaded
      program (Q13), thread path on Linux (Q14), name collisions (Q15).
- [ ] Reconcile the allocation counter's described ordering: runtime.md says
      relaxed, §19.4.5 says sequentially consistent.
- [ ] Mark 0013 `accepted` only here, once, with the spec chapter still to come.

## P1: Timeouts, then readiness (S for 1a, M for 1b)

Decision to write: 0014, network timeouts and readiness. Spec: additions to
[§18.6](../spec/18-host.md#186-tcp-networking).

**1a, timeouts**

- [ ] Specify receive, send, accept and connect timeouts (milliseconds; a
      documented value for "no timeout"; `0` is a poll) using the existing
      `.TimedOut` case, distinct from a connection reset.
- [ ] Windows: socket timeouts or `WSAPoll` with a deadline. Linux: the `ppoll`
      (or `poll`) system call, x86-64 and AArch64 numbers; no libc. macOS stays a
      placeholder.
- [ ] Tests: the timeout fires with no data; data arriving first wins; zero polls;
      an invalid value is rejected. Loopback, single thread.
- [ ] Give `example/web-server` a per-connection idle deadline so a silent client
      is dropped, with a cap on request-head time.

**1b, readiness (optional; only if a single-thread server is wanted for itself)**

- [ ] Specify `tcpPoll` over several handles with a hard cap on the set size and
      no unbounded allocation.
- [ ] Implement it on `WSAPoll` and `ppoll`; add tests: a ready subset, a closed
      peer reads as ready, zero timeout, invalid sets.
- [ ] Event-loop rewrite of the example. It needs array element removal and a
      consuming element accessor, so it waits on P2. Stage 2 supersedes it, so keep it
      small or skip it.
- [ ] Optional, Linux only: document `SO_REUSEPORT` multi-process serving (needs a
      listen option).

## P2: Language prerequisites (M to L)

Decision to write: 0015, noncopyable compiler-known types and owned handles.
This is a user-visible feature, so the feature workflow applies in full.

- [ ] Specify that some compiler-known types are **noncopyable by decree** (no user
      `deinit`), and that §4.1.1's propagation makes arrays, structs, enums and
      optionals containing them noncopyable.
- [ ] Implement noncopyable types through the type checker and semantic analysis
      (copy is an error; use-after-consume already exists). The compiler has no such
      machinery today.
- [ ] Make the `System` handle types (`TcpConnection`, `TcpListener`) noncopyable,
      so a handle cannot become two owners. Update spec §18.6, the tests and the
      example.
- [ ] Resolve 0012's open question 1: let a `match` pattern binding on an owned
      temporary be consumed, or give another way to own a received value, so the
      worker loop and `tcpCloseConnection(consume ...)` work without `?`.
- [ ] Add `Array` element removal (`removeLast`) and a consuming element accessor
      (§4.5.1), which any array of tasks or of connections needs.
- [ ] Fixtures per stage, written failing first, with stable diagnostic IDs.

## P3: Thread primitives and a minimal lock (L)

Decision to write: 0016, thread creation and blocking.

- [ ] Windows: `CreateThread`/`ExitThread`, `WaitOnAddress`/`WakeByAddress*`;
      only `kernel32`-family imports.
- [ ] Linux x86-64 and AArch64: `pthread_create`/`pthread_join` and a condition
      variable or `futex` through libc, which the program already links
      ([0011](../decisions/0011-runtime-without-the-c-library.md)). The AArch64 path
      stays untested until it runs natively. Raw `clone` (own stack and TLS,
      `futex`, and an inline-assembly trampoline intrinsic because the child starts on
      an empty stack) is deferred.
- [ ] macOS: libSystem `pthread`; written, gated until run on a Mac.
- [ ] A thread-pointer intrinsic (`FS`/`GS`, `TPIDR_EL0`, the TEB) and a way for
      the thread creator to set it, if any per-thread runtime state is needed.
- [ ] A small internal runtime API (create a thread running a runtime function with
      one argument, join, wait and wake on an `Int` address, yield) and a minimal
      spin-then-yield lock built on it and the existing atomic add. Keep it out of
      `System`.
- [ ] Decide stack size, thread-count limit, and whether either is configurable
      (0013 Q10).
- [ ] Tests: a thread starts and returns; many short threads; a write before exit is
      seen after join; wait and wake without a lost wakeup; thread-pointer isolation;
      the symbol-import gates still allow only the listed names on Windows and Linux.

## P4: Make the runtime thread-safe (M to L)

Decision to write: 0017, runtime thread safety. Every item must leave a
single-threaded program's behavior and the leak counter unchanged.

- [ ] Baseline first: record the runtime benchmark, allocation-heavy and `print`-heavy
      fixtures, and the hello-world binary size, so every later change is measurable.
- [ ] Audit every runtime global in `lib/Runtime/core/`: static slots 0 to 1
      (counter, allocator), 2 to 10 (streams and scratch), 32 to 39 (entry scratch),
      slot 16 (write once before any thread); decide per-program, per-thread, or
      locked for each.
- [ ] Allocator: one lock around the free lists and regions, keeping the block
      layout. Measure the cost of taking it only once a second thread exists against
      always taking it; cross-thread frees are legal because the size arrives with
      `free`.
- [ ] Standard streams: one `print` atomic; standard input serialized; flush rules
      kept. Initialize or guard every lazily initialized piece of state (stdout error
      state, console check, the Windows stdin binary-mode switch, the first
      socket-start call).
- [ ] Panic and exit: a global panicking flag so other tasks cannot print,
      allocate or write after the flush; the first trap wins; output flushed once; no
      destructors run; the panic path takes no lock its caller may hold, including an
      out-of-memory panic inside the allocator lock.
- [ ] `runProcess` on Linux forks: keep the child path from allocating before
      `execve`, or serialize it against the allocator lock; decide Q13.
- [ ] Constants: a constant array passed by `consume` is cloned, never freed. Add a
      test.
- [ ] Spec §19.4.5 gains any new runtime-only intrinsics; update the symbol gates.
- [ ] Gate: no regression above an agreed threshold on the baseline benchmarks and
      no hello-world size growth beyond a stated limit.
- [ ] Tests: the full existing suite passes unchanged; allocator stress freeing
      across two threads returns the counter to zero; `print` atomicity; first-use
      initialization races from two tasks.

## P5: Compiler support for tasks and channels (L)

Decision to write: 0018, task and channel types in the compiler. Feature workflow:
spec, failing tests first, every pipeline stage, role reviews, validation.

- [ ] **First:** the spec chapter. Tasks, single-sender channels, `spawn`, `join`,
      `send`, `receive`, `close`, the `consuming` rules, the memory-model rules from
      0013, and the diagnostics. Wording uses labeled calls:
      `spawn(function: serve, input: consume connection)`.
- [ ] Reserved words: `async`, `await` and `actor` stay reserved; decide whether
      `spawn` is a function name or a keyword (a function-like builtin is preferred).
- [ ] Parser: the grammar for the `function:` position (it must not be a general
      value) and any labeled-call additions; syntax nodes carry spans.
- [ ] Name resolution: resolve the `function:` argument to a function symbol;
      define builtin-name shadowing (a user `Task` wins, 0013 Q15).
- [ ] Type checking: `Task<R>` and the channel ends as compiler-known builtins;
      target rules (exactly one `consuming` parameter, non-generic, not a method,
      not native, not overloaded, not `main`, not `Never`); noncopyable ends and
      tasks; moved values dead after `spawn` and `send`.
- [ ] Semantic analysis: `task-not-joined` through a linear flag on the builtin
      with the existing consumption analysis; use-after-move across `spawn` and
      `send`.
- [ ] IR and verifier: spawn, join, send, receive, close operations; ownership
      operations stay explicit; debug scopes preserved.
- [ ] Backend: lower to the P3 and P4 runtime calls; keep the C ABI unchanged.
- [ ] Fixtures per stage under `tests/` plus GoogleTests citing the spec sections,
      failing first; end-to-end fixtures under `tests/e2e/`.
- [ ] Stable diagnostic IDs for every rejection listed in 0013.

## P6: Runtime semantics (M)

- [ ] Task runtime: start, result slot, join, the trap path, destruction of the
      task's locals before `join` returns.
- [ ] Bounded single-sender, single-receiver channel: ring buffer, a blocking send
      when full and receive when empty, close from either end, a closed result for a
      receive with no live sender, an unsent-value error on a closed send, queued
      values destroyed by the last end dropped.
- [ ] Moves versus copies: a `consume`d heap payload is not cloned (assert with the
      allocation counter); an aggregate is still copied into the argument block.
- [ ] Memory-order tests: a result visible after join, a value after receive, a
      closed channel after all earlier sends.
- [ ] Stress: many tasks, many messages, cross-thread frees, closing during traffic,
      a trap in one task while others run, a task blocked on receive when its sender
      drops. Repeat in CI, Debug and Release.
- [ ] A sanitizer or race-detector run of the runtime on Linux if a toolchain is
      already installed (automation does not install one).

## P7: Use it in the web server (S to M)

- [ ] Worker-pool version of `example/web-server`: the accept loop owns one bounded
      channel per worker and moves each `TcpConnection` into one, round-robin; workers
      close what they own; the sender is closed before the join (0013 destruction rule).
- [ ] Decide dispatch: a try-receive or small `select` so a slow connection does not
      hold up its worker's queue (0013 Q11), or document the head-of-line cost.
- [ ] Keep the per-connection deadlines from P1a.
- [ ] Measure requests per second and tail latency against the single-thread server
      and record it honestly in the example README, with the machine.
- [ ] Document the one-owner rule the example relies on, and that Markdown pages are
      read per request (no shared cache, by design).

## P8: Documentation and acceptance (M)

- [ ] Update the spec index, the preamble if goals change, `supported-features.md`,
      the portable skill's copy of it, and `docs/impl/runtime.md`.
- [ ] Run the role reviewers separately from the author: `language-designer`,
      `type-system-reviewer`, `ir-backend-reviewer`, and `adversarial-tester` on the
      new surface.
- [ ] `python3 scripts/validate.py --llvm-root <sdk>` on Windows x64 and Linux
      x86-64, Debug and Release; report unrun gates (AArch64, macOS) explicitly.
- [ ] Mark the decisions `accepted` and link the spec sections.
- [ ] Delete this plan, keeping anything still open in a smaller plan.

## Deferred, not forgotten

- Multi-sender channels, which need a runtime sender count and would be the one
  named exception to §4.1 and §4.8.
- Structured scopes that lend read-only borrows to child tasks.
- `actor` syntax as sugar over a task and its channel.
- `async`/`await`, only if tens of thousands of simultaneous connections need it.
- Per-thread allocator caches with lock-free remote frees, and raw `clone`/`futex`
  threads if the C library is ever dropped on Linux.
- Unbounded channels, if a real consumer needs them.
- A readiness API that scales past a poll set (`epoll`, IOCP, `kqueue`).
- TLS: it needs a transport abstraction, not concurrency, and is its own decision.

## Risks

| Risk | Mitigation |
|---|---|
| Noncopyable machinery (P2) is spec-only today and touches several stages | Do it as its own feature first; it also fixes copyable handles in the networking API |
| The allocator lock slows single-threaded programs | Baseline in P4 before any change; measure "lock only when threaded" against "always lock" |
| A pattern binding cannot be consumed (0012), blocking the worker loop | P2 resolves it before P5 |
| A forked `runProcess` child in a threaded process inherits held locks | P4 constrains the child path or serializes it |
| Thread-affine native state | The binding author documents it (§19.6); no type-level marker until a real case appears |
| `spawn` forces a decision on closures | Keep it a restricted position, defined as a subset of the later forms (0013 option 7) |
| AArch64 and macOS cannot be verified on the usual machines | State them as untested in every record and spec section, as 0011 does |

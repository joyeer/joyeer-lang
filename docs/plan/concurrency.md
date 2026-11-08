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
P0 design ─ P1a timeouts (done) ─ (P1b readiness, optional)
          ├ P2 language prerequisites ──────────────────────────┐
          └ P3 thread primitives + lock ─ P4 thread-safe runtime ┴ P5 compiler ─ P6 runtime semantics ─ P7 server ─ P8 acceptance
```

- P1a needs no language change; it is done (receive and accept timeouts).
- P2 (noncopyable and linear types, closing handles, owning a received value,
  array removal) has no dependency on threads. It and P4 must both finish before
  P5 starts.
- P3 builds the thread primitives and a minimal lock before P4 uses the lock;
  the first draft ordered these the other way around.
- P4 must not slow or change any single-threaded program.

## P0: Settle the design (S)

- [x] Re-run the `language-designer` review on the revised 0013 (second pass done;
      its findings are folded into 0013).
- [x] Decide the 0013 open questions that block P5: task joining (Q1), the form of
      the function reference (Q2), closing (Q4), bounded only (Q5), single sender
      (Q7), the `receive` result (Q8), zero-parameter targets rejected (Q9), channel
      creation through `initializing` outputs (Q17), linear `JoinHandle` and ends
      (Q18), ticket lock (Q19).
- [x] Decide the questions that can wait but must be recorded: `runProcess`
      rejected while tasks exist (Q13), `pthread` first (Q14), rarer names with a user
      declaration winning (Q15). Limits (Q10) are decided in P3 and dispatch (Q11) in
      P7; output ordering (Q12) stays unspecified.
- [ ] Reconcile the allocation counter's described ordering: runtime.md says
      relaxed, §19.4.5 says sequentially consistent. Do this in P4 with the audit.
- [x] Mark 0013 `accepted` as a design direction (2026-11-08), with the spec chapter
      still to come. The later decisions (0015 onward) are each marked accepted when
      their phase lands.

## P1: Timeouts, then readiness (S for 1a, M for 1b)

Decision to write: 0014, network timeouts and readiness. Spec: additions to
[§18.6](../spec/18-host.md#186-tcp-networking).

**1a, timeouts**

Done in [0014](../decisions/0014-network-timeouts.md): receive and accept take
`timeoutMillis` (`-1` unlimited, `0` poll, up to 2147483647), using a readiness
wait before the call.

- [x] Specify receive and accept timeouts (milliseconds; `-1` for none; `0` is a
      poll) using the existing `.TimedOut` case, distinct from a connection reset.
- [x] Windows over `WSAPoll`; Linux over the `ppoll` system call (x86-64 and
      AArch64 numbers; AArch64 untested); macOS stays a placeholder.
- [x] Tests on Windows and Linux: the timeout fires with no data and the
      connection stays usable; data after a timeout is not lost; zero polls; end of
      stream is not a timeout; out-of-range values are rejected; accept times out
      and returns a pending connection.
- [x] `example/web-server` reads a request head with a 5 s per-read timeout,
      answers a silent client with `408` and keeps serving; checked on Windows and
      Linux (the silent client was dropped after 5.0 s).
- [ ] Send and connect timeouts. They need non-blocking sockets (a blocking
      `send` of a large buffer waits after readiness), so they move to 1b or later.
- [ ] A total deadline for a request head. It needs a monotonic clock in
      `System`, which does not exist yet; until then a peer that dribbles bytes is
      bounded only by the 16 KiB head cap.

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

Decision: [0015](../decisions/0015-noncopyable-and-linear-types.md), proposed. Settled
2026-11-08: handles close through a real `deinit`, as the spec already describes
(§3.7, §4.1.1, §4.7), not through a special annotation. This is a user-visible feature,
so the feature workflow applies in full. The compiler today has only the `deinit`
keyword; copies are made implicitly in lowering (`prepareOwned`), consumption is tracked
one way in `InitializationBuilder`, and destroy helpers are generated per type.

**P2a: `deinit` and noncopyable types**

- [x] Settle the 0015 fork (`deinit`).
- [ ] Spec: amend §3.7 (struct-only `deinit` rules, implementation status), §4.1.1
      (propagation to containers and aggregates; `Copyable` false), §4.7 (no partial moves
      out of a type with a `deinit`) and §18.6 (handles).
- [ ] Failing tests first, one set per stage, citing the spec sections.
- [ ] Parser: `deinit() { ... }` in a struct body; reject in an `enum`, an extension, with
      parameters, with a visibility, or twice.
- [ ] Name resolution: `self` in the body; one unnameable `deinit` per struct.
- [ ] Type checking: the body, noncopyability on the type and its propagation, `Copyable`,
      a diagnostic at each copy site (assignment, argument, return, aggregate or array
      element), no consuming `self` or moving a field out in the body.
- [ ] Semantic analysis: the body through the existing analysis; reject a partially
      consumed value of a type with a `deinit`.
- [ ] IR and lowering: lower `deinit` to an internal function; `requiresDestruction`
      true; call it once per destruction, before field destruction, on overwrite and at
      scope end; keep `prepareOwned`'s copy as an assertion.
- [ ] Backend: the generated destroy helper calls it then destroys the fields; debug
      scopes preserved.
- [ ] Stable diagnostic IDs for each rejection.
- [ ] Build and run the compiler tests on Linux in WSL as well as on Windows (a C++
      change needs a rebuild; the Windows build is run from an MSVC Developer shell).

**P2b: handles that close, owning what you receive, array removal**

- [ ] `System`: `deinit` on `TcpConnection` and `TcpListener` that closes an open
      handle; the explicit close functions close, report, and store an invalid handle so
      `deinit` finds nothing (0015 question 6 checks that a consumed value may be
      mutated). Audit what noncopyability changes: `Result<TcpConnection, NetworkError>`,
      `Optional` handles and arrays of handles, `System`'s internal copies, the tests, the
      web-server example, and `Copyable` (§20.5).
- [ ] Resolve 0012's open question 1: let a `match` pattern binding on an owned
      temporary be consumed, so the worker loop and `tcpCloseConnection(consume ...)` work
      without `?`.
- [ ] Add `Array` element removal (`removeLast`) and decide on the consuming element
      accessor (§4.5.1), which any array of tasks or of connections needs.

**P2c: linear types and builtin generics**

- [ ] Specify **linear** (non-droppable) types for `JoinHandle` and the channel ends:
      amend §4.7, which destroys every owning binding at scope end; define propagation
      through `Array`, `Optional`, `Result`, structs and enums (the generated destroy
      helpers must reject a non-empty array of tasks, not drop it); and the inverse of the
      current analysis, "still initialized at scope exit". Decide how early `return`, `?`,
      loops and assignment over a live value behave, and whether the reserved `defer` is
      needed (0013 Q18, 0015 question 2).
- [ ] Amend spec §2.6 for the new compiler-known generic builtins (`JoinHandle<R>`,
      `ChannelSender<T>`, `ChannelReceiver<T>`, `SendError<T>`). Channel creation is
      decided (0013 Q17): `channel(capacity:, sender: &s, receiver: &r)` with two
      `initializing` outputs; specify it, including a capacity below 1.
- [ ] Fixtures per stage, written failing first, with stable diagnostic IDs.

## P3: Thread primitives and a minimal lock (L)

Decision to write: 0016, thread creation and blocking.

- [ ] Windows: `CreateThread`/`ExitThread` with `SRWLOCK` and `CONDITION_VARIABLE`
      for blocking, all in `kernel32`. Not `WaitOnAddress`, which is in
      `Synchronization.lib` and outside the link line and 0011's import gate.
- [ ] Linux x86-64 and AArch64: `pthread_create`/`pthread_join` and a condition
      variable through libc, which the program already links
      ([0011](../decisions/0011-runtime-without-the-c-library.md)). `pthread_create`
      takes four arguments and returns a value, so bind it with `@c_extern` and `Int`
      arguments or add an intrinsic; the entry is a Joyeer function reached through
      `symbol_address`; amend §19.6 (foreign-thread entry). The AArch64 path stays
      untested until it runs natively. Raw `clone` (own stack and TLS, `futex`, and
      an inline-assembly trampoline intrinsic because the child starts on an empty
      stack) is deferred.
- [ ] macOS: libSystem `pthread`; written, gated until run on a Mac.
- [ ] No thread-pointer intrinsic and no per-thread runtime state in v1: libc owns
      the thread pointer under `pthread`. A task's context travels through its entry
      argument and the panicking flag is global.
- [ ] A small internal runtime API (create a thread running a runtime function with
      one argument, join, block and wake on a condition, yield) and a minimal lock.
      Decide the lock (0013 Q19): a ticket lock over the existing atomic add (adding
      zero as the load), or a new compare-and-swap or atomic-load intrinsic in
      §19.4.5. Keep the API out of `System`.
- [ ] Decide stack size, thread-count limit, and whether either is configurable
      (0013 Q10).
- [ ] Tests: a thread starts and returns; many short threads; a write before exit is
      seen after join; wait and wake without a lost wakeup; the symbol-import gates
      allow exactly the added names, and only for programs that use tasks, on Windows
      and Linux.

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
- [ ] Parser: no new production for `function:`. The parser sees an ordinary labeled
      argument (`call_arg` takes any identifier); the restriction is a type-checker
      rule on the `spawn` builtin. Add only what the chosen channel-creation form
      needs; syntax nodes carry spans.
- [ ] Name resolution: resolve the `function:` argument to a function symbol;
      define builtin-name shadowing (a user `Task` wins, 0013 Q15).
- [ ] Type checking: `Task<R>` and the channel ends as compiler-known builtins;
      target rules (exactly one `consuming` parameter, non-generic, not a method,
      not native, not overloaded, not `main`, not `Never`); noncopyable ends and
      tasks; moved values dead after `spawn` and `send`.
- [ ] Semantic analysis: `task-not-joined` using the linear-type rule specified and
      implemented in P2; use-after-move across `spawn` and `send`.
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

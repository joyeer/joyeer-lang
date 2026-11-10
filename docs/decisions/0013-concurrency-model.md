# 0013: Concurrency model: share-nothing threads and channels

- Status: accepted (2026-11-08) after two `language-designer` reviews, and implemented
  (2026-11-10): tasks by [0018](0018-tasks.md), channels by [0019](0019-channels.md) (which
  amend two details below), on the runtime of [0016](0016-thread-primitives.md) and
  [0017](0017-runtime-thread-safety.md). The recommendations marked *recommended* below are
  accepted as written, and the answers to questions 17 to 19 are recorded under "Resolutions".
  Question 11 (dispatch) stays open; the web server's pool documents the cost.
- Spec: [§21 Concurrency](../spec/21-concurrency.md), [§2.6.3](../spec/02-types.md), and the
  reserved keywords in [§15](../spec/15-reserved.md) (`async`, `await` and `actor` stay reserved).

## Context

The first network API ([0012](0012-tcp-networking.md)) is blocking, so a
program serves one connection at a time and one silent client stalls every
other. The language has no threads, no closures, no `async`, and no user
generics. Concurrency is the next constraint on the web server and on any
service Joyeer is meant to replace legacy software with.

What the memory model gives us, and what it forbids:

- Every value is independently owned. Assignment copies (§4.1); `consume`
  transfers ownership (§4.2.3). There are no reference types, no pointers, no
  `Box`/`Rc`/`Arc`, and no weak references (§4.8).
- An unqualified parameter is a *borrow*, not a copy (§4.2). Access is lexical:
  a borrow or `inout` projection cannot be stored or returned, and the spec says
  a borrow does not escape to another thread (§4.8, "Cross-thread sharing
  requires a separately specified lifetime and concurrency model").
- Exclusivity is checked: while a value is borrowed or mutated, no conflicting
  access exists.
- There is no mutable global storage; top-level `let` constants are immutable
  compile-time data.
- Destruction is deterministic, compiler-managed, and in reverse declaration
  order (§4.7). There is no user `deinit` yet, so a resource handle cannot
  release itself.
- A type is noncopyable only by declaring `deinit` (§4.1.1), and `deinit` is not
  implemented. The compiler has no noncopyable machinery today.

What the implementation constrains:

- The runtime allocator is single-threaded by design (0011): size-class free
  lists, 1 MiB regions, no block header (the size arrives with `free`). The
  allocation counter is atomic. The runtime fails a program whose final
  allocation balance is not zero (runtime.md), which is a fact any exit rule must
  respect.
- Runtime state is wider than the allocator: static slots 0 to 1 (counter,
  allocator), 2 to 10 (streams and 64 bytes of shared scratch) and 32 to 39
  (entry scratch); slot 16 (`envp`) is written once before the program runs.
  Standard output has an error state and a console check, and Windows standard
  input switches to binary mode on first use (§18.5). Lazy first-use
  initialization is a race unless it is guarded.
- On Windows the runtime has no C library, so there is no `pthread`. On Linux it
  keeps the C startup and links libc (0011), though `System` makes raw system
  calls. macOS calls libSystem.
- The handle types of `System` (`TcpConnection`, `TcpListener`) are plain
  structs holding an `Int`, with no `deinit`. By §4.1.1 they are copyable, so
  nothing stops a program from copying one (0012, open question 1).
- A `match` pattern binding is not an owning local, so `consume`-ing a value
  received in a `match` arm is rejected today (0012). Postfix `?` works only
  between the same error type.
- Foreign code makes no thread-safety claim, and callbacks and entry from
  foreign threads are separate designs ([§19](../spec/19-c-interop.md)).

Goals this must serve: no GC, deterministic destruction, a model a reviewer can
check from signatures, and one way to do each thing.

## Options considered

1. **Shared memory with locks** (`Mutex<T>`, atomics, `Arc`). The conventional
   answer, but it needs shared ownership, which §4.8 removes by design. Adding
   it would reintroduce reference counting and aliasing, the two things the
   language was shaped to avoid. Rejected.
2. **Goroutines on a garbage-collected heap.** Rejected: there is no GC.
3. **`async`/`await` with colored functions.** The keywords are reserved. It
   needs function values, user generics (`Future<T>`), and a compiler pass that
   turns functions into state machines, and it splits every API into two colors.
   Deferred, not rejected.
4. **Processes only** (several server processes sharing one port, or
   `runProcess` workers). Works today and is a fair scale-out step, but cannot
   share work inside one program and has no pipes yet. A stopgap, not the model.
5. **Single-threaded event loop with timeouts and readiness polling.** No
   language change, removes the stalled-client problem, cannot use more than one
   core. Adopted as stage 1, with timeouts first.
6. **Share-nothing OS threads, ownership moved between them, bounded
   channels.** Because values have no aliases, a moved value cannot create one.
   Adopted as stage 2.
7. **A protocol-bounded generic `Work` instead of a function reference** (§20.1).
   The idiom the spec already chose for "no closures": a struct carries its input
   and has a `consuming func run()`. Cleaner in the end, but it needs §20
   generics, which are not implemented. Kept as the later form of `spawn`; the
   v1 special form must be a subset of it.

## Decision

Joyeer's concurrency is **share-nothing**: threads run concurrently, and data
crosses from one to another only by being moved through a task argument, a task
result, or a channel. There is no shared mutable state in user code and no lock.

### What "no Send/Sync" does and does not mean

Moving a value never creates an alias, so **memory safety needs no `Send`/`Sync`
distinction**: there is no `&T` two threads can hold, and a borrow cannot escape.
That claim covers memory only.

- **Handles.** A handle that denotes an operating-system resource is safe to move
  only if it cannot also be copied; copying it into two tasks would make two owners
  of one resource, and no check would catch it. Making handles safe is therefore a
  **prerequisite** of stage 2, not a follow-up: the language needs noncopyable
  types, which §4.1.1 already ties to `deinit`, with the propagation to arrays,
  structs and enums containing them, and the `System` handle types use it. This is
  the same problem as 0012's open question 1 and should be solved once. It is not
  only a copy rule: a channel destroys queued values, and a `TcpConnection`
  destroyed without a `deinit` frees its memory but leaks its file descriptor. So
  the handles close through a real `deinit` ([0015](0015-noncopyable-and-linear-types.md),
  which settles this; the earlier idea of compiler-generated destroy helpers for
  decreed handle types is dropped).
  Making a handle noncopyable also makes `Result<TcpConnection, NetworkError>`,
  `Optional` handles and arrays of handles noncopyable (§4.1.1), changes how match
  bindings and `?` treat them, and must fail the `Copyable` builtin of §20.5.
- **No native-address deny-list.** An earlier draft proposed rejecting values that
  hold a native address. That cannot be a type rule, because runtime units hold
  addresses and descriptors as plain `Int` (§4.9, §19.4.5). Native bindings that keep
  thread-affine state document it themselves (§19.6).
- **Not covered.** Race freedom is about memory. It does not cover files, ports,
  the working directory, child processes, or deadlock.

### Stage 1: timeouts, then readiness (no language change)

- **1a, timeouts.** Add receive, send, accept and connect timeouts to
  [§18.6](../spec/18-host.md#186-tcp-networking) (milliseconds; `.TimedOut` is
  already a case). This is small and defeats a silent client on its own.
- **1b, readiness (optional).** A wait over several handles, for example
  `tcpPoll(connections:, listener:, timeoutMillis:)`, backed by `WSAPoll` on
  Windows and `ppoll` on Linux. It lets one thread serve many connections, but it
  is the largest throwaway: stage 2 supersedes the event-loop rewrite of the
  example, and an event loop needs array element removal and a consuming element
  accessor that do not exist yet (`Array.removeLast` is unimplemented; §4.5.1 is
  spec-only). Do it only if a single-threaded server is wanted in its own right.

Write each as its own decision when it starts.

### Stage 2: threads, tasks and channels

- **Tasks.** `spawn(function: name, input: consume value)` starts a thread that
  runs a named top-level function. Calls use labels (grammar: `call_arg ::= label
  ':' [ '&' | 'consume' ] expression`). `name` must resolve to exactly one
  top-level function that is non-generic, not a method, not `@c_extern` or
  `@c_export`, not overloaded, not `main`, with **exactly one `consuming`
  parameter**, and a result type that is not `Never`. A `borrowing`, `inout` or
  `initializing` parameter is rejected (a borrow cannot outlive `spawn`; the child
  would mutate the parent's storage). Whether a zero-parameter function is allowed
  is an open question. The result is a `JoinHandle<R>`.
- **`join`.** `JoinHandle<R>` has a consuming `join` returning the function's owned
  result `R`. A `JoinHandle` is noncopyable (by the rule above) and **must be consumed
  on every path**: a task that is not joined is a compile error
  (`task-not-joined`). This settles process exit: every task is finished before
  `main` returns, so the leak check sees a settled balance. There are no detached
  tasks.
  - "Must be consumed" is a **linear** rule, stronger than noncopyable (affine).
    It contradicts §4.7 as written, where every owning binding is destroyed at
    scope end, so the spec needs a new kind of type that cannot be dropped, and
    §4.7 must be amended. The existing analysis tracks "uninitialized after
    consume"; the inverse check, "still initialized at scope exit", is new work.
    P2 of the plan owns it.
  - It interacts with control flow: every `?` or early `return` between `spawn`
    and `join` needs a join on that path (and `defer` is reserved, §15); a loop must
    consume per iteration; assigning over a live task is an error; and linearity
    propagates through `Array`, `Optional`, `Result`, structs and enums, so the
    generated destroy helpers must reject, not silently drop, a non-empty `[JoinHandle]`.
    Joining on an error path while the paired sender is still live is the
    deadlock described under Destruction, so the clumsy path is also the dangerous
    one.
  - An alternative to evaluate: join-on-drop (as scoped threads do) needs no linear
    type and fits §4.7, but a drop that blocks has the same hidden deadlock. The
    worker-pool example decides between them.
- **Not a function value in v1.** The spec has no closures (§20.1) and gives
  non-generic functions a `function_type` (§2.3), which the compiler does not
  implement; a generic function cannot be named as a value (§20.4). So the
  `function:` argument is an ordinary labeled argument to the parser (`serve` is a
  plain identifier, and `call_arg` cannot restrict it); it is a **type-checker
  rule**, on a builtin with no declared signature, that resolves the identifier to
  a function symbol and is not a general function value. It is specified as a subset
  of a future `function:` argument of the §2.3 function type, so every program it
  accepts stays valid then. The `Work` form (option 7) is spelled differently
  (`spawn(work: ...)`), so it is a separate form and inherits no such guarantee.
  That keeps the closure question open.
- **Channels (v1: single sender, single receiver).** *(Amended by [0019](0019-channels.md): `send` returns
  `Result<Void, ChannelClosed>` and destroys an unsent value instead of `SendError<T>`, and queued values
  are destroyed when the receiver closes, outside the channel lock.)* A bounded channel has two
  noncopyable owned ends, each with exactly one owner; capacity is at least 1.
  `send(value: consume T)` returns `Result<Void, SendError>` whose error carries
  the unsent value (the value was consumed, so it cannot vanish). `receive()`
  returns `Result<T, ChannelClosed>` (recommended; an `Optional` is the
  alternative). Dropping or closing an end closes the channel: a blocked peer wakes
  with the closed result, and values still queued are destroyed by whichever end
  drops last, on that thread, through per-type destroy helpers the compiler
  already generates for builtins. The two ends share one runtime-owned block that
  the last end dropped frees: an internal atomic count of two. That is shared
  ownership in the runtime, not user-visible aliasing, and it is a difference of
  degree from a multi-sender count, which is deferred as the one named exception to
  §4.1 and §4.8 rather than because it is categorically different. The server
  shape below does not need it.
- **Creating a channel is not yet specified.** There is no `channel(capacity:)` and
  no return type. The element type cannot be inferred from a return type alone
  (§20.4 forbids it; explicit type arguments are deferred, §20.10), a pair of
  noncopyable ends needs consuming destructuring that does not exist (there are no
  general tuples), and §2.6 allows only four generic builtins, so `JoinHandle<R>`,
  `ChannelSender<T>`, `ChannelReceiver<T>` and `SendError<T>` need a spec amendment. See open
  question 17.
- **Compiler-known types.** `JoinHandle<R>`, the two channel ends and `spawn` are
  builtins in the manner of `Array`, `Dict`, `Optional` and `Result`; they do not
  wait for user generics. Their names (`JoinHandle`, `ChannelSender`,
  `ChannelReceiver`) are chosen to be rare in application code; a user declaration
  of the same name still wins over the builtin, and that must be specified and
  tested.
- **Server shape.** The accept loop owns the listener and one bounded channel per
  worker, and moves each accepted `TcpConnection` into a worker's channel in
  round-robin order. Workers close what they own. A slow connection can hold up the
  queue behind its own worker, which is why a non-blocking try-receive or a small
  `select` is an open question.

### Stage 3 (not decided here)

Structured scopes that may lend read-only borrows to children (the exclusivity
checker already guarantees no writer while a borrow is live), actor syntax as
sugar for "a value owned by one task, driven by its channel", multi-sender
channels, and async only if tens of thousands of simultaneous connections justify
it.

### Memory-model rules (to become normative on acceptance)

- **Data-race freedom for user memory by construction.** No two tasks can name the
  same storage, except through runtime-owned channel state, which has one owner at
  each end.
- **Ordering.** These are sequentially consistent happens-before edges: `spawn`
  before the first statement of the task; the end of the task, including the
  destruction of its locals, before the `join` that returns its result; a `send`
  before the `receive` that obtains its value; closing the sending end, after all
  earlier sends, before the receiver observes the closed result. Users get no
  relaxed atomics and no fences. Output from different tasks with no such edge
  between them has no specified order.
- **Moves and copies.** A `consume` argument transfers heap storage without
  cloning, but an aggregate is still copied into the thread's argument block, so
  "no copy" holds for heap payloads, not for the value's own bytes. This is
  asserted with the allocation counter. An unqualified argument elsewhere is a
  borrow and is not allowed for a spawn target. Immutable top-level constants are
  safe to share, but a constant array passed to `spawn` is cloned, never freed.
- **Destruction.** A value is destroyed by the task that owns it when it goes out
  of scope. Reverse declaration order (§4.7) can deadlock a program that joins a
  task while still holding the sender its loop waits on; the rule is to close the
  sending end (by consuming it) before the join, and the example demonstrates it.
  Once user `deinit` exists, a `deinit` may run on a thread other than the one that
  created the value; that must then be specified.
- **Traps.** A trap in any task terminates the whole process with no destructors
  run on any task, as a trap does today (0009, 0011): a global *panicking* flag is
  set so other tasks cannot print, allocate or write after the flush; the first
  trap wins when two occur together; output is flushed once; the panic path never
  takes a lock its caller may already hold (an out-of-memory panic inside the
  allocator lock). Resources in flight are released only by the operating system.
- **Exit.** `main` returns only after every task has been consumed (above), so the
  allocation-balance check runs on a quiescent program and sees no queued or
  in-flight value.
- **Blocking.** Receive on a channel with no live sender returns the closed result
  rather than blocking forever. Bounded channels can still form a wait cycle, which
  is a deadlock the language does not prevent; this is stated, not hidden.
- **Native code.** Calling foreign code from a task has no thread-safety
  guarantee (§19). A native-binding module used from tasks documents what it needs.

### Runtime changes the model requires

- **Allocator.** Blocks carry no header, so a block can be freed on another thread
  provided the size is right. First cut: one lock around the size-class lists.
  Taking it only once a second thread exists adds a load and a branch to every
  allocation and free, so that is benchmarked against a baseline, not assumed free.
  Per-thread caches with lock-free remote frees are a later optimization.
- **Streams and lazy initialization.** One `print` is atomic with respect to
  others; standard input reads are serialized; flush-before-read and
  flush-before-panic stay. Every lazily initialized piece of state (stdout error
  state, the console check, the Windows stdin binary-mode switch, the first socket
  start) is initialized before the first thread starts or guarded.
- **Counter ordering.** The counter is described as a relaxed atomic in runtime.md
  and as sequentially consistent in §19.4.5. Reconcile them; the balance is read
  only when no other task runs.
- **No per-thread runtime state in v1.** With `pthread` on Linux and macOS the
  thread pointer belongs to libc, so the runtime does not set it. A task's context
  travels through its entry argument, and the panicking flag is global. An
  allocator cache, which would need real thread-local storage, is a later
  optimization.
- **Locking primitive.** Only an atomic add exists today (§19.4.5). A lock built on
  it is a ticket lock, with the same add (adding zero) as the load; a spin loop on a
  plain load can be hoisted by the optimizer. Otherwise a compare-and-swap or atomic
  load intrinsic is added to §19.4.5. The plan names which.
- **Thread creation.**
  - Windows: `CreateThread`, with `SRWLOCK` and `CONDITION_VARIABLE` for blocking.
    These are in `kernel32`, which the link already names. `WaitOnAddress` lives in
    `Synchronization.lib`, which the link line and 0011's import gate do not allow.
  - Linux and macOS, where libc is already linked: `pthread_create`, `pthread_join`
    and a condition variable or `futex` through libc. `pthread_create` takes four
    arguments and returns a value, but §19.4.5 has only `call_void1` and
    `call_void2`, so it is an `@c_extern` binding with `Int` arguments or a new
    intrinsic; the thread entry is a Joyeer function whose address comes from
    `symbol_address`, and §19.6, which says entry from foreign threads needs a
    separate design, is amended. Programs that use tasks import the `pthread`
    names and everyone else does not, so the Linux symbol gate (0011) is extended
    by exactly those names for task programs only (older glibc also needs
    `-lpthread`). This is cheaper and safer than
    raw `clone`, which would give the thread a TLS block without libc's layout and
    crash on the first stack-protector load, `errno` access, or support-function
    call. A raw `clone`/`futex` path, which needs a new inline-assembly trampoline
    intrinsic because the child resumes on an empty stack, is a later optimization
    if the C library is ever dropped on Linux.
  - Thread count and stack size are bounded and documented (open question).
- **`runProcess`.** On Linux it forks. In a multithreaded process the child
  inherits locks held by other threads, so the child path must not allocate before
  `execve`, or the call must be serialized against the allocator lock. Whether
  `runProcess` is allowed at all in a multithreaded program is an open question.

## Consequences

- Easier: a reviewer can see from a signature what crosses a thread boundary,
  because only `consuming` arguments, results, and channel messages do, provided
  handles are noncopyable. There is no lock order to audit and no data race on
  user memory to reproduce.
- Harder: programs that want shared mutable data must own it in one task and talk
  to it by messages, or copy it. Moving a large structure is cheap; copying is a
  deep clone and shows up in profiles.
- Harder: stage 1b without closures is verbose. It is explicit and reviewable.
- Harder: every task must be joined on every path, so an early `return` or `?`
  between `spawn` and `join` needs a join on that path. Clumsy, but explicit.
- Risk: pthread threads inside a runtime that has no libc on Windows, and the
  AArch64 and macOS paths, are the least testable here. They are separated so the
  language rules can be settled first.
- Needs first (prerequisites that are not part of this decision): noncopyable
  compiler-known types and noncopyable handles; consuming a pattern binding or a
  `?`-free way to own a received value (0012 question 1); and, for any
  array-of-tasks program, a consuming element accessor or removal from an array.
- Tests for stage 2, one per rule, each citing its spec section:
  - Ordering: a result is visible after `join`, a value after `receive`, every
    spawn input in the child, a closed channel after all earlier sends.
  - Moves: a `consume`d heap payload is not cloned (allocation counter); a moved
    binding is a use-after-consume error; a constant array passed to `spawn` is
    cloned, not freed.
  - Rejections: one fixture per diagnostic below, with stable IDs.
  - Noncopyable: copying a task or a channel end is an error; `[ChannelSender<T>]`
    propagates; a copied `TcpConnection` is rejected once handles are noncopyable.
  - Exit and traps: a trap in one task flushes once and exits, with no
    destructors; two simultaneous traps print one message; the leak check passes
    after every task ends; a channel dropped with queued values has zero balance; a
    task blocked on receive when its sender drops wakes with the closed result.
  - Runtime: allocator stress with cross-thread frees returns the counter to zero;
    `print` atomicity; first-use initialization races from two tasks; a
    `runProcess` from a task while others allocate.
  - Names: a user `struct JoinHandle` coexists with, and wins over, the builtin.
  - Deadlock: a cycle of full bounded channels hangs and is documented, or a
    timeout variant exists.
  - Gates: the symbol-import gates still allow only the listed names.
- Diagnostics needed, with stable IDs:
  - a spawn target that is not a named top-level function;
  - a target parameter that is not `consuming` (borrowing, `inout`, `initializing`),
    or zero or several parameters;
  - a target that is a method, generic, native, overloaded, `main`, or returns `Never`;
  - use of a moved value after `spawn` or `send` (the existing use-after-consume);
  - `task-not-joined`;
  - copying a task or a channel end;
  - a `function:` argument that is not a function name.

## Open questions

Questions 1 to 16 carry a recommendation, accepted as written on 2026-11-08, with
these clarifications: question 9 is **rejected** (exactly one `consuming`
parameter); question 13 is **rejected while tasks exist** (`runProcess` is not
allowed in a threaded program in v1); question 14 is `pthread` first; question 15
is the rarer names, with a user declaration winning; questions 10 (limits) and 11
(dispatch) are decided in the phases that need them (P3 and P7). Questions 17 to 19
were answered the same day; see "Resolutions" after question 19.

1. **Task joining.** Recommended: every task must be consumed on every path, a
   compile error otherwise; no detached tasks. Detached tasks would fail the leak
   check.
2. **Form of the function reference.** Recommended: the restricted `function:`
   position above, defined as not a value and as a subset of a later `Work` form.
3. **`join` result.** Recommended: plain `R`. A trap aborts the process, so `join`
   never observes a failure; widening to `Result` later would break callers, so a
   recoverable-panic design is a separate, later decision.
4. **Closing.** Recommended: the compiler-generated drop of an end closes the
   channel (it needs no user `deinit` because the compiler already generates
   destroy helpers for builtins), plus an explicit consuming `close`.
5. **Bounded or unbounded.** Recommended: bounded only, capacity at least 1; an
   unbounded queue turns a stalled consumer into memory exhaustion.
6. **`deinit` and noncopyable.** Originally recommended and accepted: user `deinit`
   not required first, handles closed by compiler-generated destroy helpers.
   **Revised 2026-11-08:** `deinit` is implemented first
   ([0015](0015-noncopyable-and-linear-types.md)), because a closing handle is the
   same feature as `deinit` and a second, special mechanism would be replaced by it.
   Handles are noncopyable and close through `deinit` before they cross threads;
   `JoinHandle` and the channel ends remain compiler-known linear builtins with
   runtime-generated destroy helpers.
7. **Multi-sender channels.** Recommended: not in v1 (shared ownership; see above).
8. **`receive` result.** `Result<T, ChannelClosed>` (recommended) or `Optional<T>`.
9. **Zero-parameter spawn targets.** Recommended: rejected, so the arity of `spawn`
   does not depend on its target; exactly one `consuming` parameter.
10. **Limits.** Maximum live threads and a stack size, and whether either is
    configurable.
11. **Dispatch.** Is a non-blocking try-receive, or a small `select`, needed so the
    accept loop can pick an idle worker?
12. **Output ordering.** Is the order of `print` across tasks without a
    happens-before edge left unspecified (recommended), or serialized?
13. **`runProcess` in a multithreaded program.** Allowed with the child path
    constrained (above), or rejected while tasks exist?
14. **Thread path on Linux.** `pthread` first (recommended) or raw `clone`.
15. **Name collisions.** Recommended: rarer builtin names (`JoinHandle`,
    `ChannelSender`, `ChannelReceiver`) in place of `Task`, `Sender`, `Receiver`,
    which application code uses constantly, plus a rule that a user declaration of
    the same name wins and a test for it.
16. **Where does a task's unread result live** if a program is stopped by a trap?
    Recommended: nowhere; a trap exits the process.
17. **Creating a channel.** The form and its exact return type are unspecified (see
    Channels): a function returning a pair needs general tuples or consuming
    destructuring, and the element type needs an annotation or an explicit type
    argument. Candidates: a builtin struct holding both ends with consuming
    extraction, or one `channel(capacity:)` call whose ends are bound through two
    consuming accessors. Blocking for stage 2.
18. **Linear types.** Whether "task must be consumed" is a new linear type kind
    (amending §4.7 and the propagation rules), or join-on-drop with its hidden
    deadlock; how early return, `?`, loops and arrays of tasks behave; and whether
    the language needs a scope-exit hook such as the reserved `defer`.
19. **Locking primitive.** A ticket lock over the existing atomic add, or a new
    compare-and-swap or atomic-load intrinsic (see Runtime changes).

### Resolutions of 17 to 19 (accepted 2026-11-08)

17. **Creating a channel** uses the language's existing `initializing` parameters,
    so it needs no tuples and no new inference rule:

    ```joyeer
    var sender: ChannelSender<Int>
    var receiver: ChannelReceiver<Int>
    channel(capacity: 8, sender: &sender, receiver: &receiver)
    ```

    `channel` is a builtin returning `Void`; both outputs are `initializing`, so the
    compiler requires both variables to be declared, uninitialized, with their
    types, and to be initialized on every returning path; the element type comes
    from those declared types, and the two must agree. A capacity below 1 is a
    runtime-checked error (`InvalidInput`-style trap or `Result`; the spec chapter
    decides which). The ends are then moved with `consume` as any value.
18. **Linear types.** `JoinHandle` is **linear**: it must
    be consumed on every path, and the language gains a "still initialized at scope
    exit" check and an amendment to §4.7. Join-on-drop is rejected for v1 because a
    blocking drop hides the destruction-order deadlock. How early `return`, `?` and
    loops interact, and whether the reserved `defer` is needed, are settled in
    decision 0015 and the spec chapter, not here.

    **Amended 2026-11-09 (decision 0015, "Linear types (P2c)", after a language-design review):**
    the channel ends are **not** linear. `ChannelSender<T>` and `ChannelReceiver<T>` are
    noncopyable and close in their destroy helper, like `TcpConnection`; an explicit consuming
    `close` remains. Only `JoinHandle<R>` is linear, so a worker's `?` needs no close of its ends.
    A linear type may not be the element type of a channel (a queued value would be dropped when
    the channel closes). The diagnostic `task-not-joined` is `semantic-analysis.linear-not-consumed`.
19. **The lock** is a **ticket lock over the existing atomic add**, using the add
    (of zero) as the load; no new intrinsic in v1. A compare-and-swap intrinsic is
    added only if measurement shows the ticket lock's fairness or cost is a
    problem.

## Evidence

- §4.8 and its design note: cross-thread sharing is explicitly left to a separate
  lifetime and concurrency model; this is that proposal.
- 0011: the allocator is single-threaded on purpose and its counter is atomic only
  because it already was; Linux keeps the C startup and libc.
- 0012: a blocking network API makes a stalled client the first concrete problem,
  and it shows both that `consuming` close works only on owning locals and that
  handles are copyable today.
- The `language-designer` review (2026-11-08) found that the first draft
  overstated the memory argument for handles, relied on a native-address deny-list
  that cannot exist, assumed noncopyable and `deinit` machinery that is not
  implemented, proposed a reference-counted sender, conflicted with the leak
  check, and wrote an invalid call form. Each is corrected above. A second pass the
  same day confirmed those fixes and added: channel creation is unspecified; "must
  be consumed" is a linear rule that §4.7 forbids as written; the `function:`
  position is a type-checker rule, not a grammar position; `WaitOnAddress` and the
  `pthread` imports conflict with the import gates; per-thread state conflicts with
  libc's use of the thread pointer; and a noncopyable handle without a closing
  destroy helper leaks. Those are the new text and open questions 17 to 19.
- Hylo (mutable value semantics) and Rust's `Send` rules are the reference points
  in the preamble. Rust needs `Sync` because it has shared references and `Arc`;
  Joyeer's difference is that it has neither, but it still needs handles that
  cannot be copied, as Rust's `MutexGuard` cannot be sent.

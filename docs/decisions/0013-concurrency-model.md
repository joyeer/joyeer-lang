# 0013: Concurrency model: share-nothing threads and channels

- Status: proposed (2026-11-08). Design only; nothing is implemented, and no
  spec chapter changes until this is accepted. Stage 1 below needs no language
  change and can be built first.
- Spec: none yet. When accepted, the rules go in a new chapter next to the
  memory model ([§4](../spec/04-memory.md)) and the reserved keywords in
  [§15](../spec/15-reserved.md) are revisited.

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
- Access is lexical: a borrow or `inout` projection cannot be stored or
  returned, so it cannot outlive the call, and the spec says a borrow does not
  escape to another thread (§4.8, "Cross-thread sharing requires a separately
  specified lifetime and concurrency model").
- Exclusivity is checked: while a value is borrowed or mutated, no conflicting
  access exists.
- There is no mutable global storage; top-level `let` constants are immutable
  compile-time data.
- Destruction is deterministic and compiler-managed. There is no user
  `deinit` yet, so a resource handle (a socket, a task) cannot release itself.

What the implementation constrains:

- The runtime allocator is single-threaded by design (0011): size-class free
  lists, 1 MiB regions, no block header (the size arrives with `free`). Only the
  allocation counter is atomic (`joyeer_intrinsic_atomic_add`, sequentially
  consistent).
- Standard output and standard input are shared runtime buffers (0011); the
  entry keeps `argv`/`envp` in static slots.
- On Windows and Linux the runtime has no C library (0011), so there is no
  `pthread`, no `errno`, and no thread-local storage from libc. Linux makes raw
  system calls; Windows calls `kernel32`; macOS calls libSystem.
- Foreign code makes no thread-safety claim, and callbacks and entry from
  foreign threads are separate designs ([§19](../spec/19-c-interop.md)).

Goals this must serve: no GC, deterministic destruction, a model a reviewer can
check from signatures, and one way to do each thing.

## Options considered

1. **Shared memory with locks** (`Mutex<T>`, atomics, `Arc`). The conventional
   answer, but it needs shared ownership, which §4.8 removes by design. Adding
   it would reintroduce reference counting and aliasing, the two things the
   language was shaped to avoid, and every reviewed function would again need
   its callees traced. Rejected.
2. **Goroutines on a garbage-collected heap.** Rejected: there is no GC.
3. **`async`/`await` with colored functions.** The keywords are reserved. It
   needs closures or function values, user generics (`Future<T>`), and a
   compiler pass that turns functions into state machines, and it splits every
   API into two colors. Heavy for the benefit before the simpler tools exist.
   Deferred, not rejected.
4. **Processes only** (several server processes sharing one port, or
   `runProcess` workers). Works today with the memory model untouched, and is the
   right scale-out step for a server. It cannot share work inside one program
   and has no pipes yet, so it is a stopgap, not the model.
5. **Single-threaded event loop with readiness polling and timeouts.** Needs no
   language change and removes the stalled-client problem for I/O-bound
   servers. It cannot use more than one core, and without closures each
   connection becomes an explicit state record. Adopted as stage 1.
6. **Share-nothing OS threads, ownership moved between them, bounded
   channels.** Because values have no aliases, a moved value is safe to hand to
   another thread with no `Send`/`Sync` distinction in user code. Adopted as
   stage 2.

## Decision

Joyeer's concurrency is **share-nothing**: threads run concurrently, and the
only way for data to cross from one to another is to be moved or copied through
a task argument, a task result, or a channel. There is no shared mutable state
in user code and no lock.

### Stage 1: timeouts and readiness (no language change)

Extend `System`'s networking ([§18.6](../spec/18-host.md#186-tcp-networking))
with receive and accept timeouts and a readiness wait over a set of handles, for
example `tcpPoll(connections:, timeoutMillis:)`, backed by `WSAPoll`/IOCP on
Windows and `ppoll`/`epoll` on Linux. This is a single thread serving many
connections and is enough to defeat a stalled client. Each connection is an
explicit state value held in an array. Write this as its own decision when it
starts; it is listed here because it is the cheapest step and changes nothing
below.

### Stage 2: threads, tasks and channels

- **Tasks.** `spawn(work, input)` starts a thread that runs a named top-level
  function. `work` is a non-capturing reference to a function; `input` is passed
  `consuming`, so the caller loses it (or passes an explicit copy). The result
  is a `Task<R>` that the caller later `join`s, receiving the function's owned
  result. A task has no access to its parent's locals, since none can be named.
- **Channels.** A bounded channel has two noncopyable owned ends, a sender and
  a receiver. `send(value: consuming T)` moves a value in; `receive()` moves one
  out. A second sender is made with an explicit `clone` (a new owned end), never
  by copying, so each end always has exactly one owner. The channel's internal
  state is the one piece of shared mutable memory and is owned by the runtime,
  like the allocation counter, behind atomics and a futex or an equivalent.
- **No `Send`/`Sync`.** Every ordinary Joyeer value is safe to move between
  threads: it is a tree with no aliases. The only exceptions are values that
  hold a native address (FFI) or a borrowed projection, and neither can be
  stored in a value, so the compiler needs a short deny-list, not a trait system.
- **Compiler-known types.** `Task<R>`, the channel ends, and `spawn` are
  compiler/runtime builtins in the manner of `Array`, `Dict`, `Optional` and
  `Result`. They do not wait for, and imply nothing about, user-defined generics.
- **Server shape.** The accept loop owns the listener and moves each accepted
  `TcpConnection` through a channel to a fixed pool of worker tasks. The
  blocking `System` calls are reused unchanged, and a stalled connection ties
  up one worker, not the program.

### Stage 3 (not decided here)

Structured scopes that must join their tasks before exit and may lend read-only
borrows to children (the exclusivity checker already guarantees no writer while
a borrow is live), actor syntax as sugar for "a value owned by one task, driven
by its channel", and async only if tens of thousands of simultaneous
connections justify it.

### Memory-model rules (to become normative on acceptance)

- **Data-race freedom by construction.** No two tasks can name the same storage
  mutably or at all, except through the runtime-owned channel state.
- **Ordering.** All of these are sequentially consistent and form
  happens-before edges: the call to `spawn` before the first statement of the
  task; the task's last statement before the `join` that returns its result; a
  `send` before the `receive` that obtains its value. Users get no relaxed
  atomics and no memory fences.
- **Moves and copies.** Passing `consuming` transfers the heap storage with no
  copy. Passing an ordinary argument or result performs the language's normal
  semantic copy (a deep clone of heap-backed values, §4.1), whose cost is the
  caller's to see.
- **Destruction.** A value is destroyed by the task that owns it when it goes
  out of scope. A task's locals are destroyed when it ends.
- **Panics.** A trap (checked arithmetic, bounds, explicit panic) in any task
  terminates the whole process after flushing standard output, as a trap does
  today. Recovering from a task panic needs unwinding that respects
  deterministic destruction and is out of scope.
- **Process exit.** Returning from `main` ends the process. Because a task
  must be joined to be observed, and the language cannot yet force a join
  (no `deinit`, no must-consume check), the first implementation either
  requires every task to be joined through a scope or documents that unjoined
  tasks are stopped without running destructors. That choice is an open
  question below.
- **Native code.** Calling foreign code from a task has no thread-safety
  guarantee, as §19 already states. A native-binding module that is used from
  tasks must say so.

### Runtime changes the model requires

- **Allocator.** Blocks have no header, so a block can be freed on a different
  thread from the one that allocated it, provided the size is correct. First
  cut: one global lock around the size-class lists. Then per-thread caches over
  a central pool, with lock-free remote frees, as in mimalloc. The decision
  point is a benchmark of allocation-heavy programs; the runtime benchmark
  already covers the single-thread case.
- **Streams.** `print` and the stdout buffer are guarded so one `print` is
  atomic with respect to others. Standard input reads are serialized.
- **Per-thread state.** Anything the runtime keeps per program that a thread
  must not share (an allocator cache, a panic context) needs thread-local
  storage. Without a C library that is the thread pointer register (`FS` or `GS`
  on x86-64, `TPIDR_EL0` on AArch64, `TEB` on Windows) set by whoever creates the
  thread.
- **Thread creation and blocking.** Windows: `CreateThread`, `WaitForSingleObject`
  or `WaitOnAddress`. Linux, with no libc: `clone` with `CLONE_VM | CLONE_THREAD`
  and the related flags on a stack the runtime maps, `CLONE_SETTLS` for TLS, and
  `futex` for blocking. macOS: libSystem `pthread`. These are new
  system-call-level intrinsics beyond the one added in 0011.

## Consequences

- Easier: a reviewer can see from a signature what crosses a thread boundary,
  because only `consuming` arguments, results, and channel messages do. There is
  no lock order to audit, no `Mutex` to forget, and no race to reproduce.
- Harder: programs that want shared mutable data must either own it in one task
  and talk to it by messages, or copy it. Moving a large structure is cheap
  (an ownership transfer), but copying is a deep clone and shows up in profiles.
- Harder: writing event-loop code in stage 1 without closures is verbose. It is
  explicit and reviewable, which is the stated trade.
- Risk: raw `clone`/`futex` threads and TLS without libc are the least
  conventional part, and the AArch64 and macOS paths are the least tested. They
  are separated so stage 1 and the language rules can be settled first.
- Tests for stage 2 when it is built: ordering (a result is visible after
  `join`, a value after `receive`), moved values never observed by the sender,
  channel bounds and closing, allocator stress with cross-thread frees and the
  leak counter, panic in a task terminating the process with its output
  flushed, `print` atomicity, and the symbol-import gates still passing.
- Diagnostics needed: a spawn target that is not a named top-level function, an
  argument that is not `consuming` where required, use of a moved value after
  `spawn` or `send`, a task result that is never joined (if the scope rule is
  adopted), and a value containing a native address crossing a thread boundary.

## Open questions

1. Whether every task must be joined through a structured scope, or detached
   tasks are allowed and stopped at process exit. Structured is safer and
   matches deterministic destruction, but needs a must-consume check the
   language does not have.
2. Whether the function reference for `spawn` is a general non-capturing
   function type (a step toward closures) or a compiler special form that only
   `spawn` understands. The special form is smaller and does not decide the
   closure question.
3. Whether `join` yields the result directly or a `Result` for a future
   recoverable-panic design. Starting with a plain result keeps the door open
   only if the type can later widen without breaking callers.
4. How closing works: a channel with no senders left should end the receiver's
   loop, which needs a way to know the last sender is gone, and a sender has no
   `deinit` to say so. Explicit `close(consuming sender)` is the likely answer.
5. Whether a bounded channel suffices or an unbounded one is also needed, given
   unbounded growth is exactly what a stalled consumer would cause.
6. How much of this waits on `deinit`, which would make handles and tasks
   release themselves and could replace several of the explicit-close rules in
   this record and in 0012.

## Evidence

- §4.8 and its design note: cross-thread sharing is explicitly left to a
  separate lifetime and concurrency model; this is that proposal.
- 0011: the allocator is single-threaded on purpose and its counter is atomic
  only because it already was, which tells us where the first change lands.
- 0012: the blocking network API makes a stalled client the first concrete
  problem, and `consuming` close and `?` unwrapping already model one-owner
  handles.
- Hylo (mutable value semantics) and Rust's `Send` rules are the reference
  points in the preamble; the difference here is that the absence of references
  removes the need for a `Sync` concept altogether.

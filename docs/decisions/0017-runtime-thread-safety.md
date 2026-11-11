# 0017: Runtime thread safety

- Status: accepted (2026-11-09). Phase P4 of the [concurrency plan](../plan/concurrency.md),
  built on [0016](0016-thread-primitives.md). It makes the runtime safe to call from several
  threads; it adds no language feature and changes no single-threaded behaviour.
- Spec: [§19.4.5](../../specs/19-c-interop.md) gains three runtime-only intrinsics (atomic load,
  exchange and compare-and-exchange). The memory-model facts the runtime keeps are those
  [0013](0013-concurrency-model.md) states in prose.

## Context

P3 can start a thread, but a started thread must not allocate, print or panic: the runtime was
written for one thread (the allocator says so, 0011). This decision audits every piece of shared
state in `lib/Runtime/core/`, assigns each one a protection, and builds the locks. It must leave a
program that never starts a thread running the same code with negligible extra cost, because most
programs are single threaded and the runtime is the zero-cost-abstractions promise's floor.

The first draft of this decision chose a ticket lock over the atomic add, one streams lock, and a
5% / 4 KiB gate. Review and measurement changed all three (see "What changed from the draft").

### The audit

| State | Where | Written by | Verdict |
|---|---|---|---|
| leak counter | slot 0 | every alloc and free | atomic add (sequentially consistent); unchanged |
| allocator state: bump pointer, region end, free-list heads, lazy first region | slot 1 | `allocate`, `reallocate`, `release` | **allocator lock** (slot 12) |
| large-block mappings | the OS | `take`, `give` | lock-free: each is its own mapping |
| output state block (mapped on first use), stdout buffer, terminal flag | slot 2 | `print`, `putBytes`, flush | **output lock** (slot 13) |
| output system-call scratch (Windows `written`, Linux termios) | slots 3 to 7 | `systemWrite`, `systemTerminal` | output lock |
| input state block (mapped on first use), stdin buffer | slot 15 (new) | `stdinReadByte`, discard | **input lock** (slot 14) |
| input system-call scratch | slot 8 | `systemRead` | input lock |
| panic scratch | slot 9 | `panic` | only the thread that claimed the panic flag |
| thread count | slot 11 | the thread API | atomic; read by every lock site |
| environment block address, string scratch, argument handle, byte buffer | slots 16, 32 to 37 | the entry, once, before `main` | write once before any thread exists |
| panicking flag | slot 38 (new) | `panic` | atomic claim |

Two claims in the draft were wrong and are corrected: the Windows stdin binary-mode switch does not
exist in the runtime (reads use `ReadFile` directly), and the Windows socket start is reference
counted by the operating system, so neither needs a runtime guard. `System`'s own first-use state is
P5's concern (below).

## Decision

### Locking is conditional on there being a second thread

A lock site first loads slot 11, the number of unjoined threads (0016). If it is zero the program is
single threaded: nothing is locked and the site costs one load and one branch (inlined at the site,
since a call for it measured slower). The count is raised by the thread that starts a thread before
the thread exists and lowered by `join` after it has finished, so it is non-zero exactly while
another thread can run, and the transitions are ordered by thread creation and join themselves. The
site remembers whether it locked (`let held = threadsActive()`) and unlocks only if it did, so a
critical section that straddles a change of the count cannot unlock a lock it never took. "Always
lock" would cost an atomic read-modify-write per allocation for a feature most programs do not use.

### The locks: operating-system assisted, on three new atomic intrinsics

The draft's ticket lock spun and yielded. It collapses under oversubscription and a blocked reader
would have held the streams lock for its whole wait, so the runtime now uses **sleeping locks**:

- Linux: a three-state futex mutex (0 free, 1 held, 2 held with waiters) built on the new
  `joyeer_intrinsic_atomic_cmpxchg` and `joyeer_intrinsic_atomic_exchange`, waiting and waking with
  the raw `futex` system call (202 on x86-64, 98 on AArch64; operations 128 and 129, private).
- Windows: `SRWLOCK` (`AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`,
  `TryAcquireSRWLockExclusive`) and `Sleep` for parking.
- macOS: `os_unfair_lock_lock`, `unlock` and `trylock`, and `sleep` for parking.

The new intrinsics (sequentially consistent, 8-byte, runtime-only like `atomic_add`):
`joyeer_intrinsic_atomic_load(address)`, `joyeer_intrinsic_atomic_exchange(address, value)` returning
the old value, and `joyeer_intrinsic_atomic_cmpxchg(address, expected, desired)` returning the old
value. The core's allowed imports grow by exactly the names above; the symbol gates are updated.
The lock word is the slot's own storage; the code lives in `core/locks.joyeer`:
`lockBlocking`, `lockUnblock`, `lockTry`, `parkThread`, `threadsActive`, `panicking`.

Locks are not recursive and are **leaf locks**: nothing holds one while acquiring another or while
allocating. Critical sections are a few instructions (a free-list pop, a buffer copy) and never call
out except for the system call a flush makes.

### The allocator

The allocator lock (slot 12) covers the small-block paths and the lazy first region: `takeSmall`
(including the state), the small branch of `give`, and the free-list and bump-pointer updates. A
large block takes no lock. `reallocate` takes the lock once per class change. Block layout is
unchanged, so a block allocated on one thread can be freed on another, provided the size is right,
which `free` already receives.

### The streams: separate output and input locks

Output (slot 13) and input (slot 14) have separate locks and separate state blocks (16 KiB output,
8 KiB input, each mapped lazily under its own lock), so a thread blocked reading standard input
never stops another thread from printing. Rules:

- A `print` is **atomic**: each exported `joyeer_print_*` function takes the output lock once and
  releases it after its newline and the flush a terminal gets, so two prints never interleave.
  `putByte`, `putBytes` and `endPrint` require the caller to hold the lock. Formatting a `Float` or
  `Double` allocates its scratch *before* the output lock and releases it after.
- The exported `joyeer_stdout_write`, `joyeer_stdout_flush` and `joyeer_stderr_write` take the output
  lock themselves; `joyeer_stdin_read_byte` and `joyeer_stdin_discard` take the input lock. A read
  that must flush standard output first takes the output lock for that flush only, never both at
  once.
- A write larger than the buffer goes straight to the system call under the lock.
- The system-call scratch belongs to the lock of the code that uses it.

### Panic and exit

The first trap wins and every other thread stops:

1. `panic` claims slot 38 with an atomic add. A thread that is not first **parks** if other threads
   exist (`parkThread`: sleeps forever, never returns, never prints), and otherwise exits at once
   (a panic inside the panic path of a single-threaded program behaves as it always did).
2. Every lock slow path checks the flag and parks, so a thread arriving at any runtime lock after the
   claim stops there and nothing prints or allocates after the flush. A thread already inside a
   critical section finishes it and parks at its next lock.
3. The first thread tries the output lock a bounded number of times (`lockTry`), then flushes
   standard output **without** it if that fails (it may itself be inside that critical section, as in
   an out-of-memory panic while mapping the block), and writes standard error with its own scratch
   (slot 9). It takes no allocator lock and allocates nothing.
4. Output is flushed once, no destructors run, the message appears once, exit status 1.

The process entry checks slot 11 at exit: a program that returns from `main` with threads still
unjoined prints `Joyeer runtime error: N thread(s) not joined` and exits 1, because the allocation
balance is not final until every thread is joined.

### `runProcess` on Linux

`runChild` forks. The report buffer is now allocated before the fork, so the child allocates nothing
before `execve`; rejecting `runProcess` while tasks exist at compile time (0013 Q13) is still P5's.

### The counter

The leak counter is a sequentially consistent atomic add (§19.4.5); `docs/impl/runtime.md`, which
said "relaxed", is corrected.

## What changed from the draft

- Ticket lock → OS-assisted sleeping locks, which required three atomic intrinsics instead of one
  system call (`sched_yield`).
- One streams lock → separate output and input locks.
- Panic: the flag claim, parking of losers and of late arrivals at any lock, and the bounded try came
  out of the adversarial review; a design that parked only at the streams lock left holes.
- The audit's stdin binary-mode and socket-start items were wrong (above).
- The gate (below) was re-measured with an interleaved method; a fixed percentage on a noisy machine
  measured nothing.

## Gates and measured results

Method: interleaved A/B runs of baseline and changed Release builds (`taskset -c 3` on Linux), the
minimum and median of N runs each, so machine drift hits both equally (`tests/perf/README.md`).

| Measure | Baseline | Final |
|---|---|---|
| `hello` executable, `-O2` (Linux x86-64) | 32,440 B | 37,960 B (+5,520 B) |
| `print_heavy` min / median | 0.14 s / equal | 0.13 s / equal |
| `alloc_heavy` min / median | 0.10 s / equal | 0.10 s / equal |
| `runtime_benchmark` allocator and string cases | within run-to-run noise | |

The hello growth is the locks, the extra intrinsics' call sites, the panic protocol and the split
streams; it is 17% of a very small number, and the real per-call cost, a load and a branch, is
inside noise. The size gate was therefore revised from 4 KiB to "no more than 6 KiB at `-O2`", and
the speed gate to "no regression outside interleaved-A/B noise".

Tests (all in the suite): `thread_safety_test.cpp` (allocator stress with cross-thread frees that
returns the counter to zero, allocate-free churn, `print` atomicity under four writers captured
through the file descriptor, a print larger than the buffer, first-use races on the allocator and the
streams, simultaneous panics and a nested panic as death tests); `RuntimeTestsThreadedMode`, which
reruns the **whole runtime suite** with one parked thread so every lock path is executed (a
single-threaded run cannot find a lock taken twice); the intrinsics' fixture and invalid-signature
tests; and `tests/e2e/const_array_consume.joyeer` (a constant array passed by `consume` is cloned,
never freed, and the balance stays zero).

## Not in this decision

- **`System`'s own first-use state and `runProcess` rejection while tasks exist**: P5, because a task
  can only call them once tasks exist.
- **Per-thread allocator caches** and lock-free remote frees: only if a baseline shows contention.
- **The Windows C-startup `/WHOLEARCHIVE` link** imports the thread functions into every such
  program (0016); still to decide.

## Consequences

- Every runtime function is callable from any started thread.
- A single-threaded program pays a load and a branch at each lock site and 5.5 KB of code.
- Critical sections are tiny and sleeping locks do not collapse under oversubscription; contention on
  the allocator is the signal to add per-thread caches, not to change the lock.
- `joyeer_panic` gains a flag and a bounded try.

## Evidence

- The audit is `lib/Runtime/core/{allocator,streams,print,panic,entry,locks}.joyeer`.
- 0016's review measured the ticket lock unfair under oversubscription (8 s at 16 threads on 16
  vCPUs against 0.5 s at 12), which is why this decision does not use one.
- Baselines and the A/B method: `tests/perf/` and `runtime_benchmark`.

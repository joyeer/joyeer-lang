# 0016: Thread primitives

- Status: accepted (2026-11-09), implemented, amended the same day after a backend review that ran the
  mechanism on Windows x64 and Linux x86-64. Phase P3 of the
  [concurrency plan](../plan/concurrency.md). It decides the runtime's lowest layer for threads;
  there is no language surface here. The language-level tasks and channels are
  [0013](0013-concurrency-model.md), built on this in P5 and P6.
- Spec: amends [§19.6](../spec/19-c-interop.md) (a thread the runtime created entering Joyeer
  code) and fixes the static-slot pointer of [§19.4.5](../spec/19-c-interop.md). No intrinsic is
  added.

## Context

0013 fixed the model (share-nothing threads, tasks, bounded channels) and left the runtime work
to P3 and P4. Today the runtime is single threaded by design: the allocator says so
([0011](0011-runtime-without-the-c-library.md)), the only atomic is the leak counter's add, and
there is no thread, lock or wait anywhere. P3 adds only what a thread needs to exist and to be
waited for; making the allocator, streams and counter safe is P4.

Facts, checked against the code:

- The runtime is Joyeer compiled as core units (scalars and addresses held as `Int`), built into
  archive objects. A core unit may declare `@c_extern` bindings with scalar parameters (six work;
  C `int`, `DWORD` and `BOOL` results are `Int32` or `UInt32`, widened with `as Int`, because the
  upper half of the register is undefined), and `@c_export` functions whose address `symbol_address`
  takes in the same unit.
- **Archive members are not always linked lazily.** Linux and the Windows own-entry link are:
  a member nothing references does not reach the program. The Windows C-startup link uses
  `/WHOLEARCHIVE` and macOS uses `-force_load`, which load every member, so a thread object would
  add its imports to those programs. See "Gating".
- Static slots are 64 eight-byte, zero-initialised words shared by the program. Used today: 0
  counter, 1 allocator, 2 stream state, 3 to 10 stream scratch, 16 the process environment block,
  32 to 33 string scratch, 34 to 36 the argument handle, 37 the byte buffer. The slot table is
  written in `docs/impl/runtime.md` by this decision (there was none; 0009 D1 gave only the ranges).
- User task code is not a runtime unit, so it cannot use `@c_export` or `symbol_address`
  (`typechecking.cpp`): the thread entry that P5 hands the runtime is a function the *compiler*
  knows, not an address the program computes. The thread API therefore owns the C-convention
  trampoline.

## Decision

### Scope and shape

A new core unit `lib/Runtime/core/threads.joyeer` is compiled into a **fourth archive object**,
`runtime-threads`. It exports, with the C ABI:

| Function | Meaning |
|---|---|
| `joyeer_thread_start_abi(thread, entry, argument)` | Start a thread that runs `entry(argument)`. `thread` is caller-supplied, zeroed storage of `JOYEER_THREAD_BYTES` (128). Returns 0 or a negative code. |
| `joyeer_thread_join_abi(thread)` | Wait for the thread to finish and release it. Returns 0 or a negative code. |
| `joyeer_mutex_{init,lock,unlock,destroy}_abi(storage)` | A blocking lock over an operating-system lock in caller-supplied storage of `JOYEER_LOCK_BYTES` (64). |
| `joyeer_condition_{init,wait,signal,broadcast,destroy}_abi(storage, ...)` | A condition variable that waits with a mutex; a wait can wake spuriously, so a caller waits in a loop. |

`entry` is `void (*)(void *)` on every platform: a plain function taking one address and returning
nothing, which is what `joyeer_intrinsic_call_void1` can call. The runtime's own private
`@c_export` **trampoline** has the platform's thread-routine shape (`DWORD WINAPI f(LPVOID)` or
`void *f(void *)`, a function of one `Int` returning an `Int`, which is the same machine code
shape on x86-64 and AArch64). It receives the address of the `thread` record, reads `entry` and
`argument` from it, calls `entry(argument)` with `call_void1`, writes the status, and returns. The
tests pass a C++ function; P5 will pass a function the compiler emits.

The `thread` record is 128 bytes: handle, entry, argument, status, then 64 bytes of attribute
storage (`pthread_attr_t` is 56 bytes on glibc and musl and 64 on macOS). The API never allocates
and a started thread never calls the allocator, the streams or `panic` on the runtime's behalf:
that is P4's business, and until then a function run on a started thread must not allocate.

### Per platform

| | Thread | Mutex and condition | Result mapping |
|---|---|---|---|
| Windows | `CreateThread` (stack size 8 MiB with `STACK_SIZE_PARAM_IS_A_RESERVATION`), `WaitForSingleObject(INFINITE)`, `CloseHandle` | `SRWLOCK` (`AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`) and `CONDITION_VARIABLE` (`SleepConditionVariableSRW`, `WakeConditionVariable`, `WakeAllConditionVariable`) | `WAIT_OBJECT_0` is 0; `WAIT_FAILED` (`0xFFFFFFFF`) is a failure, `-GetLastError()` |
| Linux, macOS | `pthread_attr_init`, `pthread_attr_setstacksize` (8 MiB), `pthread_create`, `pthread_join`, `pthread_attr_destroy` | `pthread_mutex_*`, `pthread_cond_*` | the call's own return, negated |

All Windows names are in `kernel32`, which the link line already names. Not `WaitOnAddress`
(`Synchronization.lib`, outside the link line and the import gate). On POSIX the program already
links libc for its startup. A raw `clone`/`futex` path is rejected for v1: the child would get a
TLS block without libc's layout and crash on the first stack-protector load, `errno` access or
support-function call. AArch64 uses the same `pthread` calls and stays untested until it runs
natively; macOS is written and gated until it runs on a Mac.

**Stack size is explicit and the same everywhere: 8 MiB**, reserved lazily (Windows with the
reservation flag, POSIX through `pthread_attr_setstacksize`). Defaults differ by up to a factor of
64 (Windows 1 MiB, macOS secondary threads 512 KiB, musl 128 KiB, glibc the process limit), so a
task that works on one must work on all. It is not configurable in v1. A stack-size static slot is
never read without a lock; the size is a constant.

### Lock storage

The operating-system lock objects are opaque and platform-sized. Caller storage is **64
bytes, 8-byte aligned, zero-initialised**, checked by `static_assert(sizeof(...) <= 64)` in a C++
test against the real headers rather than assumed. Sizes: `SRWLOCK` and `CONDITION_VARIABLE` 8;
`pthread_mutex_t` 40 on glibc x86-64 and musl, 48 on glibc AArch64, 64 on macOS; `pthread_cond_t`
48 on all of them. Zero is a valid initial state for `SRWLOCK`, `CONDITION_VARIABLE` and the glibc
and musl objects (their static initialisers are zero), so `init` stores zeros and calls
`pthread_mutex_init` and `pthread_cond_init` on macOS, where zero is not valid. `destroy` is a
no-op on Windows and calls `pthread_*_destroy` on POSIX.

### The thread limit

At most **256 unjoined threads**, counted in static slot 11. `start` does `n = atomicAdd(slot, 1)`;
if `n >= 256` it does `atomicAdd(slot, -1)` and returns `JOYEER_THREAD_LIMIT`, and a failed create
decrements too. `join` decrements, so the count means "started and not yet joined". The code
`-0x40000000` is outside the range of negated operating-system codes (a negated Win32 code reaches
only about -16000). Not configurable in v1; it makes a runaway `spawn` loop fail with a checkable
error. There is no detach in v1: P5's linear `JoinHandle` makes sure every thread is joined.

### The runtime's own lock (0013 Q19) is recorded here and built in P4

The allocator, counter and streams will need a lock that is cheap and never sleeps in the kernel.
The decision stands: a **ticket lock over the existing atomic add**, `atomicAdd(x, 0)` being the
load (verified: seq_cst, not hoisted at `-O2`, exact under 32 threads). Two points from the review
move it out of P3. Nothing in P3 calls it, so it would be dead code with no test. And without
backoff it collapses under oversubscription (a preempted waiter stalls every ticket behind it;
measured: 8 s at 16 threads on 16 vCPUs against 0.5 s at 12), so it needs spin-then-yield, which
needs `sched_yield` or `SwitchToThread` in the core or an exchange intrinsic. P4 builds it
address-parameterised (`lockAcquire(address)` over two words, so the allocator, streams and panic
each have one), with the yield decision, and its slots come from the core range 11 to 15.

### Gating and the link

- On **Linux and the Windows own-entry link**, `runtime-threads` is linked only into a program that
  references it, so hello-world and `System` programs get no thread symbol; the existing
  `RuntimeSystemProgramNeedsOnlyTheCStartup` tests guard this.
- On the **Windows C-startup link** (`/WHOLEARCHIVE`) and on **macOS** (`-force_load`) every member
  is loaded, so those programs import the thread functions even if they never use them. That is
  harmless (the functions are in `kernel32` or libSystem) but it breaks "the imports never reach
  those gates". v1 accepts it and says so; the Windows fix, a plain archive plus `/INCLUDE:wmain`,
  is deferred with a regression test. The gate tests that use whole-archive check only a minimum
  import count, so none fails.
- A new gate test allows the thread object exactly its per-platform imports, plus `joyeer_panic`
  (a bounds trap on a static slot) and defines `joyeer_static_slots`.
- `-lpthread`: a Joyeer program linked with a `pthread_create@GLIBC_2.34` reference links and runs
  without it on glibc 2.43; older glibc, musl and macOS are unverified. P5 adds a `usesThreads`
  link option and emits `-lpthread` after the archive group only then. Until then the documented
  minimum is glibc 2.34.

### The spec text (§19.6)

"A function the runtime exports as a thread trampoline may be entered by a thread the runtime
created through the thread API. Such a thread must not allocate, print or panic until the runtime
is thread safe, and nothing unwinds across the thread boundary. Entry from any other foreign thread
still needs its own design."

## Consequences

- The runtime stays single threaded for every program that does not reference the thread object.
  P4 then makes the allocator, streams and counter safe, with the lock.
- No language change and no new intrinsic.
- Testing needs no Joyeer thread entry: C++ runtime tests start threads with C++ functions, and a
  C-interop fixture runs the same on the runtime-module path, so P3 is tested on Windows and Linux
  before the compiler knows what a task is.
- The record layout and the codes are part of the C ABI header and are documented there.

## Open questions

1. **Whole-archive on Windows C-startup and macOS.** Accept the extra imports (recommended for
   v1) or switch to `/INCLUDE:wmain` and `-ObjC`-free linking.
2. **8 MiB task stacks.** Cheap as reservations, 2 GiB of address space at the limit; a smaller
   default is possible once a measurement says tasks need less.
3. **Thread names, priorities, affinity.** Not in v1.
4. **A thread-exit hook** (flush streams, release an allocator cache) is P4's.

## Evidence

- The runtime is Joyeer in four objects (`lib/Runtime/CMakeLists.txt`); the gates are in
  `tests/unittests/runtime` and `scripts/cmake/verifyRuntime*.cmake`.
- `compiler/codegen/linker.cpp`: lazy archive link on Linux and Windows own-entry,
  `/WHOLEARCHIVE` on Windows C-startup, `-force_load` on macOS.
- A runtime-module program that ran `pthread_create` and `pthread_join` on a Joyeer `@c_export`
  entry, and one that ran `CreateThread` and `WaitForSingleObject`, were built and run during the
  review.

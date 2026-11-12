# 0019: Channels

- Status: accepted as a design (2026-11-10), after a `language-designer` review whose findings are
  folded in below. Phases P5b and P6 of the [concurrency plan](../plan/0009-concurrency.md): the compiler
  support and the runtime for bounded single-sender, single-receiver channels, on top of tasks
  ([0018](0018-tasks.md)). It amends two details of [0013](0013-concurrency-model.md) (see
  "Amendments"). Implemented on Linux and Windows.
- Spec: [§21.9](../../specs/21-concurrency.md#219-channels) (new) and
  [§2.6.3](../../specs/02-types.md#263-concurrency-builtin-types-provisional).

## Context

A task can take one moved input and return one moved result. Anything more (a worker that serves many
connections, a pipeline) needs values to cross threads while both ends run. 0013 decided the shape: a
bounded channel with two noncopyable ends, each owned by one task, values moved through it, closed
when an end is dropped, created through `initializing` parameters (Q17). 0015 made the ends
noncopyable but not linear. 0016 and 0017 built the mutex, the condition and the thread-safe
allocator the runtime needs. What remains is the types, the four operations, the runtime object, and
the exact results of `send` and `receive`.

Constraints:

- No user generics, so the ends are compiler-known builtin generic types like `JoinHandle<R>`.
- An end is an opaque handle (one pointer), and the operations are IR instructions the backend lowers
  to runtime calls, as for tasks.
- A queued value is owned by the channel and must be destroyed if nobody receives it, with the
  element type's destroy helper.
- The runtime is Joyeer core code with its C ABI in `runtime.h`; it may use the thread object's mutex
  and condition, and must not allocate per message.

## Options considered

1. **What `send` returns.** (a) `Result<Void, SendError<T>>` carrying the unsent value (0013). It needs
   a generic builtin whose payload is its type argument, which the compiler has no mechanism for.
   (b) `Optional<T>`, the unsent value or `.None`: `.None` meaning success reads backwards. (c)
   **`Result<Void, ChannelClosed>`; a value that cannot be sent is destroyed.** Adopted: the same shape
   as `receive`, one new nongeneric builtin enum, and nothing a v1 caller can use is lost, since a
   noncopyable element (a handle) is closed by its destroy helper and nothing leaks. The cost is real:
   a dispatcher cannot fail an item over to another worker. Getting the value back is a later,
   **differently named** variant (a nonblocking `trySend` that returns it); changing `send`'s result type
   later would break callers. `Err` is silently ignorable (there is no must-use rule), so a statement
   `sender.send(value: consume x)` loses the failure; a warning is future work.
2. **What `receive` returns.** `Result<T, ChannelClosed>` (0013 Q8). A closed channel with queued values
   still delivers them first.
3. **Closing.** Dropping an end closes it, and a consuming `close()` is the same operation spelled out
   (0013 Q4).
4. **Where queued values die.** 0013: by whichever end drops last. **Amended:** when the **receiver**
   closes nobody can read the queue, so its values are destroyed at that moment, on the receiver's
   thread, outside the channel lock (see the close protocol); when the sender closes the
   queue stays for the receiver. The block is freed by the second end to close.
5. **One object or two.** The channel code is its own archive object (like the thread object),
   referenced only by programs that create a channel, so every other program is unchanged.
6. **Blocking.** `send` blocks while the queue is full and the receiver is open; `receive` while it is
   empty and the sender is open. No timeout in v1 (0013 Q11).
7. **Receiver of `send` and `receive`.** `borrowing`, not `inout`: an end is an I/O-like handle, like the
   `System` handles that are also borrowed; borrows cannot cross threads (§4.8), ends are unique, and all
   shared state is behind the runtime lock. `inout` would force `&` and break `receive()` in a loop over
   a borrowed parameter. The cost is that the mutation is not marked at the call site (§0.1); a note in
   the spec says why.

## Decision

### Types

- `ChannelSender<T>` and `ChannelReceiver<T>` are builtin generic types with one argument. They are
  **noncopyable** (`type-checking.copy-of-noncopyable`) and **not linear**: dropping one closes it. Each
  is one pointer at run time (the channel block's address), or null when uninitialized or moved from.
- **Element types.** `T` may be any type except: a **linear** type (`type-checking.linear-channel-element`,
  the name 0015 reserved), `Any` and `Never` (`type-checking.channel-element-type`: no layout, and
  `receive` would be ill-formed). `Void` is allowed as a pure signal (`send(value: consume ())`). A
  noncopyable element (a handle with `deinit`), an array, a `String` and a **nested end** (the
  request/reply idiom) are all allowed. The rule is checked at **every** occurrence of the types in a
  type position (a parameter, a field, an `Optional`, an array), not only at a `channel(...)` call.
- `ChannelClosed` is a builtin enumeration with one case, `Closed` (an entry in
  `hostbuiltins::enumerations`). `send` and `receive` return `Result<Void, ChannelClosed>` and
  `Result<T, ChannelClosed>`.
- A user declaration of any of these names wins in its scope. The function `channel` is a common
  identifier, so a local named `channel` shadows the builtin silently and a call then fails with
  `not callable`; the argument diagnostics of `channel` show the creation form in their help, and AI
  authors who write `let (tx, rx) = channel(capacity: 8)` get that help.

### Creating a channel

```joyeer
var sender: ChannelSender<Int>
var receiver: ChannelReceiver<Int>
channel(capacity: 8, sender: &sender, receiver: &receiver)
```

`channel` is a prelude function with the parameters `capacity: Int`, `sender` and `receiver` (both
`initializing`, 0013 Q17). The existing `initializing` machinery already checks and analyzes the
arguments (`type-checking.invalid-initializing-argument` for a wrong marker or an operand that is not
an uninitialized or wholly consumed mutable local or an `initializing` parameter;
`semantic-analysis.initializing-initialized-storage` for storage that is already live), so a user
wrapper with two `initializing` parameters of these types works too. The type checker adds, in
`checkChannel`: the two operands' types are `ChannelSender<T>` and `ChannelReceiver<T>`
(`type-checking.channel-end-types`) for the **same** `T`; `capacity` is `Int`, and an integer literal
(optionally negated) below 1 is `type-checking.channel-capacity`. Any other capacity below 1 traps
with `invalid channel capacity`, and a capacity whose buffer size overflows traps with
`channel too large`.

### Operations

- `sender.send(value: consume x)`: borrowing receiver, one labeled consuming argument of type `T`, result
  `Result<Void, ChannelClosed>`. It waits while the queue is full and the receiver is open. If the
  receiver is closed it returns `.Err(.Closed)` and the value is destroyed. A top-level constant array
  passed as `value` is **cloned**, never handed to the receiver to free (§21.2.2).
- `receiver.receive()`: borrowing receiver, result `Result<T, ChannelClosed>`. It waits while the queue
  is empty and the sender is open. It returns a queued value even if the sender has closed, and
  `.Err(.Closed)` once the queue is empty and the sender is closed. `Err(ChannelClosed)` cannot be
  propagated with `?` into a function with another error type (0013 context); the worker idiom is a
  `while true { match receiver.receive() { .Ok(v) => ..., .Err(_) => { break } } }` loop, and
  conversions are by `match`.
- `consume sender.close()` and `consume receiver.close()`: consuming builtin members that end the
  handle now; the binding is uninitialized afterwards (it may be assigned again). The end also closes
  when its owner is destroyed, on any path. An end inside an array, a struct field or an `Optional`
  closes implicitly through the destroy helpers; `consume a[i].close()` is rejected (an element path,
  as for `consumeEmpty`) and the way to close one is `removeLast()` first; `consume w.sender.close()`
  on a field of a `deinit` type is `partial-consume-of-deinit-type`.
- A blocked operation's owner cannot close that end (the end is borrowed during the call), so a close is
  only ever of the **other** end, and it wakes the blocked one.

### Ordering and memory

All operations on one channel are totally ordered by its lock; there is no ordering across different
channels beyond each thread's program order. A `send` happens before the `receive` that gets its value;
closing the sender, after all earlier sends, happens before the receiver sees the closed result. The
runtime copies only the value's own bytes into and out of the queue and never clones. A `match` that
binds the payload of `receive()` **clones a copyable payload** (a `String`, an array) until §7.1.2 moves
payloads out of owned temporaries for every type; this is the existing behavior of `removeLast`, it is
not a runtime copy, a test pins it, and moving it is a follow-up. A noncopyable payload is moved.

### The runtime object and its close protocol

`lib/Runtime/core/channels.joyeer` is a fifth archive object, referenced only by a program that uses a
channel. It defines the four exports below, uses the thread object's `joyeer_mutex_*` and
`joyeer_condition_*`, the allocator and `joyeer_panic`, and no operating-system function of its own.

```c
/* 0 on success; the capacity and stride are checked, the element destroy callback may be null */
int64_t joyeer_channel_create_abi(int64_t capacity, int64_t stride, int64_t alignment,
                                  JoyeerDestroyValueFn destroy, void** sender, void** receiver);
/* 1 if the value was queued (it now belongs to the channel), 0 if the receiver is closed (the caller still owns it) */
int64_t joyeer_channel_send_abi(void* end, const void* value);
/* 1 if a value was moved into `value`, 0 if the queue is empty and the sender is closed */
int64_t joyeer_channel_receive_abi(void* end, void* value);
/* closes the end (sender or receiver, recorded in the end handle); a null end is a no-op */
void joyeer_channel_close_abi(void* end);
```

An end handle points at the block; the sender and receiver are told apart by a tag in the low bit of the
stored pointer (blocks are 16-byte aligned), so a handle stays one pointer and `close` needs no extra
argument. The **block** is one allocation: a mutex, two conditions (not empty, not full), the capacity,
stride, head index, count, element destroy callback, a flag per closed end, then the ring buffer starting
at a 16-byte-aligned offset, with a stride rounded to the element's alignment. A zero-size element has a
zero-byte ring and no copy. The protocol, which is what makes the two ownership rules safe:

1. The count, the head, both closed flags and the decision "I am the last closer" are read and written
   **only under the block's mutex**, never with atomics.
2. `send` copies its bytes into the ring inside the critical section; `receive` copies them out inside
   it. Waits are `while (!predicate) wait` loops (spurious wakeups). There is at most one waiter per
   condition (one sender, one receiver), and a close signals the condition of the peer anyway, **before
   the mutex is released**.
3. Closing the **receiver** sets its flag, wakes the sender, and **moves** every queued value that needs
   destroying into a private buffer while it holds the mutex; it destroys them through the element destroy
   callback **after unlocking**, then frees the buffer. Moving them first matters: if it unlocked before
   moving, a sender that then closes would see both flags set, become the last closer, and free the block
   (and the queue) under the receiver's feet. Destroying after the unlock matters too: a value's destroy
   can close another channel end (an end sent through a channel), and when two channels hold each other's
   ends a destroy under the lock closes an end of the same channel and deadlocks on the non-recursive
   mutex (found by the adversarial review). An element's `deinit` therefore runs on the receiver's thread
   without the channel lock held, and may use other channels freely.
4. Closing the **sender** sets its flag and wakes the receiver; queued values stay.
5. The closer that finds the other flag already set is the last: it unlocks, calls
   `joyeer_mutex_destroy_abi` and `joyeer_condition_destroy_abi`, and frees the block with its computed
   size. The other closer touches nothing of the block after its `unlock` (POSIX allows destroying a mutex
   after another thread's unlock, and SRW locks are equivalent).
6. A nonzero status from any `joyeer_mutex_*` or `joyeer_condition_*` calls `joyeer_panic`.
7. **Self-deadlock check.** Before a wait, if the unjoined-thread count (static slot 11) is zero, no
   other thread exists that could ever wake this one, so it traps with
   `channel deadlock: nothing can wake this thread`. The check is conservative (a finished but unjoined
   thread still counts) and costs one atomic load on the blocking path only.

### IR and backend

Four instructions: `channel_create(capacity, sender_address, receiver_address)`,
`channel_send(sender_address, value) -> Result<Void, ChannelClosed>`,
`channel_receive(receiver_address) -> Result<T, ChannelClosed>` and `channel_close(end_address)` (the
consuming `close`). The verifier checks that the two addresses of `create` are ends of one `T`, that the
value type of `send` is `T` and its result is the `Result` of `Void`, that the result of `receive` is
the `Result` of `T`, and that `close` takes an address of an end. Each instruction keeps its source span
and debug scope.

- `channel_send` **consumes its operand on both outcomes**: when the runtime reports the channel
  closed the backend destroys the value with the element's destroy helper, and when it is queued the
  channel owns it. The operand must be an owned value, prepared exactly as `task_spawn`'s input.
- The `Result` is built from the runtime status as `Array.removeLast` builds its `Optional`: the tag, and
  for `Err` the payload `ChannelClosed.Closed`, which is all zero bytes. `receive` moves the bytes
  straight into the `Ok` payload.
- `channel_close` **zeroes the end's storage**, as `task_join` does, so the scope-end destroy of the
  moved-from binding is a no-op. The destroy helper of an end calls `joyeer_channel_close_abi` (null is a
  no-op); unlike a task's it never traps. Its clone helper is unreachable and traps.
- `channel_create` passes the element's destroy helper (null when it needs none), its size and its
  alignment, and zeroes nothing else: uninitialized `var`s are already zero-initialized storage.
- The IR wire format gains the instructions (the next version after 0018's 14); the backend C ABI version
  is unchanged. A program that contains a `channel_create` links the thread archive member and
  `-lpthread`: `usesThreads` (0018) is computed from `task_spawn` **or** `channel_create`, with a test.

### Interaction with tasks

An end moves into a task as (part of) its input and out as part of its result. The 0013 deadlock rule
stands: a program that joins a task while still holding **an end the task waits on** (the sender its loop
receives from, or the receiver a blocked producer needs) never ends. Close that end by consuming it
before the join. A cycle of full bounded channels hangs. These are stated, not prevented; the
self-deadlock trap above catches only the single-threaded case. A compiler **warning**
(`semantic-analysis.channel-end-live-at-join`: a join while an end of a channel created in the same
function is still possibly live, with a `consume x.close()` fix-it) is planned for P6 and is not required
to ship P5b.

### Diagnostics

`type-checking.`: `invalid-initializing-argument` and `semantic-analysis.initializing-initialized-storage`
(reused), `channel-end-types`, `channel-capacity`, `linear-channel-element`, `channel-element-type`
(`Any`, `Never`), `copy-of-noncopyable` and `invalid-consume-argument` (reused for `close` and for
`send` without `consume`), the generic-arity diagnostic `invalid-type-argument-count`, and
`semantic-analysis.use-after-consume` for use after `close`. A mismatched `send` argument is the ordinary
assignability error.

## Amendments to 0013

- `send` returns `Result<Void, ChannelClosed>` and destroys an unsent value (0013: `SendError<T>`
  carrying it); the value-returning form is a later, differently named operation.
- Queued values are destroyed when the **receiver** closes (outside the lock, from a private buffer), not
  when the last end closes.
  The `SendError<T>` builtin of §2.6.3 is dropped from v1.

## Not in this decision

Multi-sender channels, unbounded channels, `select`, `trySend`, `tryReceive`, timeouts, `SendError<T>`,
iterating a receiver, a must-use warning for `send`, moving copyable payloads out of owned temporaries in
`match`, and the worker-pool example (P7).

## Consequences

- A producer and a consumer can run concurrently with no shared memory and no lock in user code.
- Each send and receive takes one mutex; the contention is between exactly two threads.
- A channel of capacity `n` and stride `s` costs one allocation of about `n*s + 320` bytes.
- The compiler gains two builtin generic types, one builtin enum, a builtin function with
  `initializing` outputs, four IR instructions and one runtime object.

## Tests

- Runtime (white-box GoogleTests against the C ABI): ring order across wraparound; capacity 1; blocking
  send until a receive and blocking receive until a send (two threads); a close from either end wakes the
  blocked peer; queued values destroyed on receiver close, counted by the destroy callback, including
  while the sender is mid-send and while it is blocked on a full queue; queued values kept on sender close
  and delivered; both close orders free the block exactly once (the allocation counter); many create and
  close cycles; zero-size and 16-aligned elements; the bad-capacity and overflow traps; the self-deadlock
  trap; the symbol gate and the Linux and Windows import gates for the new object and for a channel-only
  program.
- Per compiler stage: name resolution (the builtins, shadowing by a user `channel`); type checking (every
  diagnostic above, element types `Any`, `Never`, `Void`, a nested end, a noncopyable element, a linear
  element inside `Optional`, an array and a struct through an annotation); semantic analysis (ends
  noncopyable and closing implicitly in a struct, an array and an `Optional`, `close` consumes, use after
  close, reassign after close, a constant array sent, a wrapper with `initializing` parameters, `senders[i]
  .send(...)` over an array of ends); IR lowering and verifier mutations; the backend object.
- End to end (Linux and Windows, `-O0`..`-O3`): a producer task and a consumer in `main` with a struct of
  an end and a count as the task input; ordering across 10000 messages; a `String`, a noncopyable `deinit`
  element and an array element (destroy counts, the leak counter at zero, and the clone-on-match behavior
  pinned); close from either end; a receive after the sender closed drains then reports closed; a
  two-stage pipeline; a capacity of one; a trap in a task while another is blocked on `receive`; the
  single-threaded full-queue self-deadlock trap; a hang harness with a timeout for the join deadlock.

## Open questions

1. **A `must-use` warning for `send` and `receive` results.** Recommended: add one with the `channel-end-
   live-at-join` warning.
2. **A documented capacity bound.** Recommended: only the overflow trap in v1.

## Evidence

- 0013 Q4, Q5, Q7, Q8, Q17; 0015 (ends noncopyable, element not linear, the reserved name
  `linear-channel-element`); 0016 and 0017 (the mutex, condition and thread-safe allocator); 0018 (the
  builtin generic type, the thread-archive link, the linear-argument rules).
- The `language-designer` review (2026-11-10): the creation form works on the existing `initializing`
  machinery; the close protocol must be written (the use-after-free window of a receiver that unlocks
  before destroying); `match` clones a copyable payload; element validity and diagnostic IDs needed
  settling.
- Go's and Rust's bounded channels have the same close semantics for the receiver draining after the
  sender closes; Rust's `send` returns the value on failure, which is the deferred variant.

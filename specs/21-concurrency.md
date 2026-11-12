## §21 Concurrency: Tasks

> **Status:** implemented (phase P5a of the [concurrency plan](../docs/plan/0009-concurrency.md), stages 0
> and 1) on Linux and Windows. Channels (§21.9) are specified with
> [decision 0019](../docs/decisions/0019-channels.md) and join this chapter when they are built. The
> design record is [decision 0018](../docs/decisions/0018-tasks.md); the model it builds on is
> [decision 0013](../docs/decisions/0013-concurrency-model.md).

### 21.1 Purpose and model

Joyeer's concurrency is **share-nothing**. A **task** is a function running on its own operating
system thread. Data crosses from one thread to another only by being **moved**: into a task as its
input, out of a task as its result, and (with channels) as a message. There is no shared mutable
state in user code, no lock, no atomic and no fence.

Moving a value never creates an alias (§4.2.3), a borrow cannot be stored or sent (§4.8), and a
top-level `let` is immutable data (§3.1.1), so a program that uses only tasks cannot race on user
memory. This guarantee is about memory. It does not cover files, ports, the working directory, child
processes, output order or deadlock.

### 21.2 Starting a task

```joyeer
func square(value: consuming Int): Int { return value * value }

func main() {
    let handle = spawn(function: square, input: consume 7)   // JoinHandle<Int>
    let answer = consume handle.join()
    print(value: answer)                                      // 49
}
```

`spawn` is a builtin function in the prelude, not a keyword, declared with the parameters `function`
and `input`. The ordinary call rules check the labels and their order
(`name-resolution.argument-out-of-order`, `argument-count-mismatch`). A declaration of the name in the current module, file or
lexical scope takes precedence over it; a name imported from another module does not (§12.3).

- `function:` is a name that resolves to one function (an identifier, or a module-qualified
  identifier). It is **not** a value: a variable is not a valid argument
  (`spawn-target-not-function`).
- `input:` is the argument for the target's only parameter. As for every `consuming` argument
  (§4.3), it must be written with `consume`, a literal or a temporary included
  (`type-checking.invalid-consume-argument`).

The call has type `JoinHandle<R>`, where `R` is the function's result type (`Void` included).

#### 21.2.1 The target

The function must be:

- a top-level function (not any method);
- not `@c_extern` or `@c_export` (§19), and not the root module's `main` (§3.2.6)
  (`spawn-target-unsupported`);
- declared with **exactly one parameter**, and that parameter is `consuming` (§4.2)
  (`spawn-target-parameters`). A `borrowing`, `inout` or `initializing` parameter is rejected because a
  borrow cannot outlive the call and a child would otherwise mutate its parent's storage. A function
  with no parameter is rejected so the shape of `spawn` does not depend on its target; wrap the input in
  a struct, or pass a unit-like (empty) struct, which occupies no storage;
- not declared to return `Never` (`spawn-target-never`).

(A generic function is also rejected: it is not a function value, §20.4.1.) A public
function of a source dependency may be a target; a private one is not visible (§12).

`function:` takes a function's name, not an arbitrary function value (§5.6.1): this is a subset of a
future `function:` argument of a function type (§2.3.1), provided `spawn` is then redeclared as a
generic builtin, and every call the compiler accepts stays valid. A function value may itself be a
task's input or result and may be sent through a channel.

#### 21.2.2 Ownership

The input is moved into the task when `spawn` commits. A named value passed with `consume` is
uninitialized afterwards (§4.2.3). A heap value (a `String`, an array, a `Dict`) is **not cloned**; an
aggregate's own bytes are copied into the task's storage and the heap parts they reference move with
it. Passing `consume pair.first` follows the ordinary partial-move rules (a type with `deinit` cannot be
partially moved). A top-level constant array (§3.1.1) passed as the input, or returned as the result, is
**cloned**, never freed by the task or the joiner. A `JoinHandle` may itself be an input or a result.

### 21.3 Joining

`JoinHandle<R>` is a compiler-known builtin type (§2.6.3) with a single member:

```joyeer
consuming func join(): R     // spelled consume handle.join()
```

`join` waits for the task to finish and returns the owned result. It is the only way to end a handle,
and like any consuming member it must be written `consume h.join()` on an owned handle (a borrowed or
`inout` handle is `invalid-consume-argument`). A `JoinHandle<R>` is **linear** (§4.1.1, §4.7.1): it must
be ended on every path out of the scope that owns it, and it cannot be dropped, copied, or overwritten
while live. A program that does not join a task does not compile
(`semantic-analysis.linear-not-consumed`, whose help names `join`), so there are no detached tasks, and
`main` returns only after every task has finished, which is what lets the allocation-balance check run on
a quiescent program (see [runtime](../docs/impl/runtime.md)).

Consequences of the linear rules:

- a `spawn(...)` statement whose handle is not used is `linear-result-dropped`;
- a `?` between `spawn` and `join` leaves the handle live, which is an error. A function that must use
  `?` takes its state by `inout` and joins in its caller (the borrowing idiom of §4.7.1), or joins first:
  `(consume h.join())?` is accepted when `R` is a `Result` (a `consume` marker covers the receiver of the call
  it starts, so it is parenthesized before a postfix `?`);
- a joined result is not an argument: `print(value: consume h.join())` is rejected because the argument
  marker and the receiver marker would both be `consume`; write `let v = consume h.join()` first, then use `v`;
- the handle must be stored in a variable before it is joined: `spawn(...).join()` is rejected, because
  `join` consumes its receiver in place;
- a struct field holding a handle ends field by field (`consume worker.handle.join()`), and a struct
  holding one cannot declare `deinit`;
- `Dict`, `for-in` and a top-level `let` cannot hold a handle.

A fan-out over several tasks keeps the handles in an array and drains it with `removeLast()` (§2.6.2),
ending the empty array with `consumeEmpty()` (§2.6.4):

```joyeer
func total(count: Int): Int {
    var handles: [JoinHandle<Int>] = []
    var index = 0
    while index < count {
        &handles.append(element: consume spawn(function: square, input: consume (index + 1)))
        index = index + 1
    }
    var sum = 0
    while true {
        let popped = &handles.removeLast()
        match consume popped {
            .Some(handle) => { sum = sum + consume handle.join() }
            .None => { break }
        }
    }
    consume handles.consumeEmpty()
    return sum
}
```

A `spawn` inside a `deinit` is allowed; it is join-on-drop and carries that hazard (§21.6).

### 21.4 Memory-model rules

These happens-before edges are **sequentially consistent**:

1. `spawn` happens before the first statement of the task.
2. The end of the task, **including the destruction of its locals**, happens before the `join` that
   returns its result.
3. The input of `spawn` is visible in the task and the result of `join` is visible to the joiner, with
   no further synchronization.

A `deinit` (§3.7.1) runs on the thread that owns the value when it is destroyed: a moved input's on the
task, a joined result's on the joiner. Output from different tasks with no such edge between them has
no specified order, but each `print` call is atomic. Users get no relaxed atomics and no fences.

A trap in any task ends the whole process: output is flushed once, the first trap wins if two occur
together, no destructor runs on any task, and resources in flight are released only by the operating
system. A task blocked in `join` is ended with the rest. There is no way to observe a task's failure
from `join`.

### 21.5 Limits and failure

At most 256 tasks may be unjoined at once, each with an 8 MiB stack; neither is configurable. When the
limit is reached, or the operating system refuses a thread, `spawn` traps with `too many tasks` or
`cannot start task`; a failing `join` traps with `cannot join task`. The trap is deliberate: a
recoverable `trySpawn` that returns the unspent input is a later addition. A stack overflow in a task is
not trapped by the language: it ends in the operating system's fault handling (the main thread's stack
size differs, 8 MiB on Linux and 1 MiB on Windows).

### 21.6 Restrictions

- **`runProcess`** and `spawn` cannot appear in the same program: the compiler reports
  `semantic-analysis.run-process-with-tasks` at the `runProcess` call, citing a `spawn`. "The program"
  is the root module and every source dependency; the test is whether a call to the prelude `spawn` and a
  call to System's `runProcess` both occur, not whether one can reach the other. It is not modular: a
  library that calls `runProcess` stops compiling in a program that spawns a task. (The Linux fork child
  no longer allocates before `execve`, so the rule is conservative, and its removal is an open question
  in decision 0018.)
- A task may call `System` operations; their shared state needs no guard (decision 0018, audit).
- Calling native code from a task has no thread-safety guarantee (§19.6). A native binding that keeps
  thread-affine state documents it.
- Destruction order (§4.7) can deadlock a program that joins a task while holding something the task is
  waiting for. With tasks alone the task can wait only on input and output, so a `join` on a task
  blocked in `accept` or a standard input read does not return. Channels add more ways (decision 0019).

### 21.7 Diagnostics

Type checking unless noted.

| ID | Rejects |
|---|---|
| `spawn-target-not-function` | `function:` is not a name that resolves to a function (a variable, an expression) |
| `spawn-target-unsupported` | the target is a method, `@c_extern`/`@c_export` or `main` |
| `spawn-target-parameters` | the target does not have exactly one parameter, or it is not `consuming` |
| `spawn-target-never` | the target returns `Never` |
| `name-resolution.argument-out-of-order`, `argument-count-mismatch`, `unexpected-argument-label` | wrong labels, order, or a missing or extra argument of `spawn` |
| `invalid-consume-argument` | `input:` without `consume`; `join` without `consume`, or on a borrowed or `inout` handle |
| `copy-of-noncopyable` | copying a `JoinHandle` |
| `linear-collection-element`, `linear-top-level-binding`, `linear-in-deinit-type` | a handle in a `Dict` or `for-in`, a top-level `let`, or a `deinit` type |
| `semantic-analysis.linear-not-consumed` | a path leaves a handle unjoined |
| `semantic-analysis.linear-overwrite`, `linear-result-dropped`, `use-after-consume` | overwriting a live handle; dropping an owned one; using a moved input |
| `semantic-analysis.run-process-with-tasks` | `runProcess` in a program that calls `spawn` |

An unknown `function:` name is name resolution's ordinary unknown-name diagnostic.

### 21.8 Grammar

No production is added: `spawn(function: f, input: consume x)` is an ordinary
call ([Appendix C](appendix-c-grammar.md), `call_arg`).
The restriction on `function:` is a type-checking rule, not a grammar rule.

### 21.9 Channels

> **Status:** implemented in phases P5b and P6 ([decision 0019](../docs/decisions/0019-channels.md)).

A **channel** carries values from one task to another. It is bounded, has exactly one sender end and
one receiver end, and moves each value: a value sent is gone from the sender and owned by whoever
receives it. There is still no shared state in user code.

#### 21.9.1 Creating a channel

```joyeer
var sender: ChannelSender<Int>
var receiver: ChannelReceiver<Int>
channel(capacity: 8, sender: &sender, receiver: &receiver)
```

`channel` is a builtin function whose last two parameters are `initializing` (§4.10): each argument is `&`
of an uninitialized (or wholly consumed) mutable local, or of an `initializing` parameter, so a wrapper
function can take two `initializing` parameters and call `channel`. The two have types `ChannelSender<T>`
and `ChannelReceiver<T>` for the same `T`, and both are initialized by the call. `T` is any type that is not
linear (§4.7.1), `Any` or `Never`; `Void` is allowed as a pure signal, and a nested channel end is allowed
(the request/reply idiom). The rule applies at every occurrence of the types in a type position. A capacity
that is an integer literal below 1 is a compile-time error; any other capacity below 1 traps
(`invalid channel capacity`), and one whose buffer size overflows traps (`channel too large`).

`ChannelSender<T>`, `ChannelReceiver<T>` and the enumeration `ChannelClosed` (one case, `Closed`) are
builtin names; a user declaration of the same name takes precedence in its scope (§21.2).

#### 21.9.2 The ends

Each end is **noncopyable** (§4.1.1): copying one is `type-checking.copy-of-noncopyable`. An end is
**not linear**: it is closed when its owner is destroyed, on every path, so a `?` or an early `return`
needs no cleanup. An end moves like any noncopyable value, into a task as its input, out as part of its
result, into an array or a struct field.

#### 21.9.3 Sending and receiving

```joyeer
match sender.send(value: consume 5) {
    .Ok(_) => (),
    .Err(_) => print(value: "the receiver is gone"),
}
match receiver.receive() {
    .Ok(value) => print(value: value),
    .Err(_) => print(value: "closed"),
}
```

- `sender.send(value: consume x)` has type `Result<Void, ChannelClosed>`. It **waits** while the queue is
  full and the receiver is open. If the receiver is closed it returns `.Err(.Closed)` and the value is
  destroyed (a handle in it is closed by its destroy helper), so nothing leaks. A top-level constant array
  passed as `value` is cloned, never freed by the receiver (§21.2.2). The `Err` can be ignored without a
  diagnostic. `send` and `receive` take their end as a **borrow**, not `inout`: an end is an I/O-like
  handle, borrows cannot cross threads, and all shared state is behind the channel's lock.
- `receiver.receive()` has type `Result<T, ChannelClosed>`. It **waits** while the queue is empty and the
  sender is open. Values still queued when the sender closes are delivered first; it returns
  `.Err(.Closed)` once the queue is empty and the sender is closed.
- Neither operation has a timeout (a bounded wait is future work, decision 0013 Q11).
- `Err(ChannelClosed)` cannot be propagated with `?` into a function with another error type; the worker
  idiom is a loop that matches:

```joyeer
while true {
    match receiver.receive() {
        .Ok(job) => handle(job: consume job),
        .Err(_) => { break }
    }
}
```

Binding the payload of `receive()` in a `match` clones a copyable payload (a `String`, an array) until
§7.1.2 moves payloads out of owned temporaries for every type; a noncopyable payload is moved.

#### 21.9.4 Closing

`consume sender.close()` and `consume receiver.close()` end a handle immediately, like destroying it; the
binding is uninitialized afterwards and may be assigned again. An end also closes when its owner is
destroyed, including an end held in an array, a struct field or an `Optional`; to close one element of an
array, remove it with `removeLast()` first (`consume a[i].close()` is rejected). Closing the sender wakes
a blocked `receive`; closing the receiver wakes a blocked `send` and destroys every value still queued, on
the closing thread after it has taken them out of the channel and released its lock, so the `deinit` of a
channel element may use other channels. The channel's storage is freed by the second end to close, which is
decided under the lock. A blocked operation's own end is borrowed for the call, so only the peer can close and wake it.

#### 21.9.5 Memory model and limits

All operations on one channel are totally ordered by the channel's lock (there is no ordering across
different channels beyond each thread's program order), so §21.4 holds: a `send` happens
before the `receive` that gets its value, and closing the sender, after every earlier send, happens
before the receiver observes the closed result. A value is moved, not copied; its own bytes are copied
into the queue and out of it.

A channel that carries its own receiver (or two channels that carry each other's receivers) leaks: the only
reference to the receiver is in the queue, so the channel is never freed. It is memory safe and the leak
check at exit reports it; the language does not detect the cycle.

A program can deadlock: every thread blocked, a cycle of full channels, or a `join` of a task waiting on an
end the joiner still holds (the sender its loop receives from, or the receiver a blocked producer needs).
Close that end (by consuming it) before the join. The language does not detect this, with one exception: a
single-threaded program (no unjoined thread) that would wait traps with `channel deadlock: nothing can
wake this thread`. A channel of capacity `n` holds at most `n` values.

#### 21.9.6 Diagnostics

| ID | Rejects |
|---|---|
| `type-checking.invalid-initializing-argument`, `semantic-analysis.initializing-initialized-storage` | an operand of `channel` without `&`, not an uninitialized mutable local or `initializing` parameter, or already initialized |
| `type-checking.channel-end-types` | the two operands are not a `ChannelSender<T>` and a `ChannelReceiver<T>` of the same `T` |
| `type-checking.channel-capacity` | a literal capacity below 1 |
| `type-checking.linear-channel-element` | an element type that is linear |
| `type-checking.channel-element-type` | an element type of `Any` or `Never` |
| `type-checking.copy-of-noncopyable` | copying an end |
| `type-checking.invalid-consume-argument` | `send` without `consume` on its value; `close` without `consume`, on a borrowed end, or on an array element |
| `semantic-analysis.use-after-consume` | using an end after `close` |

## §21 Concurrency: Tasks

> **Status:** implemented (phase P5a of the [concurrency plan](../plan/concurrency.md), stages 0
> and 1) on Linux and Windows. Channels are specified with
> [decision 0019](../decisions/0019-channels.md) and join this chapter when they are built. The
> design record is [decision 0018](../decisions/0018-tasks.md); the model it builds on is
> [decision 0013](../decisions/0013-concurrency-model.md).

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
- not `@c_extern` or `@c_export` (§19), and not `main` (`spawn-target-unsupported`);
- declared with **exactly one parameter**, and that parameter is `consuming` (§4.2)
  (`spawn-target-parameters`). A `borrowing`, `inout` or `initializing` parameter is rejected because a
  borrow cannot outlive the call and a child would otherwise mutate its parent's storage. A function
  with no parameter is rejected so the shape of `spawn` does not depend on its target; wrap the input in
  a struct, or pass a unit-like (empty) struct, which occupies no storage;
- not declared to return `Never` (`spawn-target-never`).

(A generic function is also rejected, but user generic functions do not exist yet, §3.2.3.) A public
function of a source dependency may be a target; a private one is not visible (§12).

This is a subset of a future `function:` argument of the function type (§2.3), provided `spawn` is then
redeclared as a generic builtin: every call the compiler accepts stays valid, so the closure question
stays open.

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
a quiescent program (see [runtime](../impl/runtime.md)).

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

No production is added: `spawn(function: f, input: consume x)` is an ordinary call (§17, `call_arg`).
The restriction on `function:` is a type-checking rule, not a grammar rule.

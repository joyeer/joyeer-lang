# 0018: Tasks: `spawn`, `JoinHandle` and `join`

- Status: accepted (2026-11-09), after a `language-designer` review whose findings are folded in
  below. Phase P5a of the [concurrency plan](../plan/0009-concurrency.md), the first half of the compiler
  work in [0013](0013-concurrency-model.md). Implemented: stage 0 (`consumeEmpty`) and stage 1
  (`JoinHandle`, `spawn`, `join`) on Linux and Windows. Channels are
  [0019](0019-channels.md) (P5b with the runtime of P6), so tasks ship on the P3 and P4 runtime alone.
- Spec: [§21 Concurrency](../../specs/21-concurrency.md) (new), [§2.6.3](../../specs/02-types.md#263-concurrency-library-types)
  and, as the first stage, `Array.consumeEmpty()` (§2.6.4, new).

## Context

Everything a task needs below the compiler exists: threads (0016), a thread-safe allocator, streams
and panic path (0017), linear types so a handle cannot be dropped (0015), and noncopyable handles
that close through `deinit`. What is missing is the surface: a way to name a function, give it an
owned input, get a handle, and join it for an owned result.

Constraints, from 0013 and the compiler as it is:

- No closures and no function values (§20.1): the target is a *named* function.
- No user generics: `JoinHandle<R>` is a compiler-known builtin like `Optional<T>`. (Superseded by
  [0023](0023-library-concurrency.md): `JoinHandle<R>`, `spawn` and the `task_spawn`/`task_join`
  operations are now library source; the compiler keeps only sealed compatibility checks.)
- A borrow cannot cross to another thread: the target takes one `consuming` parameter.
- The grammar already has `call_arg ::= label ':' [ '&' | 'consume' ] expression`: no new syntax.
- Each pipeline stage keeps its own model (AGENTS.md): a task is a builtin *call* through the front end
  and two new IR operations behind it.
- The whole program (the root module plus every source dependency) is compiled as one unit; there are
  no separately compiled binary modules, so a whole-program check sees everything.

## Options considered

1. **A keyword `spawn`.** Rejected: `spawn(function: f, input: x)` already parses as a call.
2. **`spawn` as a prelude function** with a type-checker rule for `function:`. Adopted. A declaration
   in the current module, file or lexical scope wins over a prelude name; a name *imported* from
   another module does not (§12.3 searches the prelude before imports, and §12.5 forbids a standard
   module from redefining a prelude name). The collision risk is theoretical: nothing in the
   repository declares `spawn` or `JoinHandle`.
3. **`consume h.join()` (method) versus `join(handle: consume h)` (function).** The method form is what
   0013 and 0015 wrote and reads like §3.2.4's `consume b.build()`. Adopted. It is more than a flag
   (see "What the compiler must grow").
4. **Thunk in the runtime, the backend, or IR lowering.** The entry must call one specific function with
   one specific argument and result layout, so something is generated per target. Adopted: **the backend
   emits it** next to the `task_spawn` it serves. The first draft put it in IR lowering so the verifier
   would check it, but IR has no raw-pointer load or offset operation to read the input out of the block,
   so such a thunk cannot be written in IR. The IR verifier checks the `task_spawn` instead (the target's
   signature), and LLVM's verifier checks the thunk.
5. **Result slot in a runtime object versus in the task block.** One heap block holds the thread record,
   the result and the input, so one allocation and one free serve a task and the leak counter needs no
   new rule. Adopted.

## Decision

### Surface

```joyeer
func square(value: consuming Int): Int { return value * value }

let handle = spawn(function: square, input: consume 7)   // JoinHandle<Int>
let answer = consume handle.join()                       // 49
```

- `spawn` is declared in the prelude with the parameters `function` and `input`; labels and their
  order are checked by name resolution's ordinary call rules (`name-resolution.argument-out-of-order`,
  `argument-count-mismatch`, `unexpected-argument-label`), so there is no new arguments diagnostic. `function:` is a name expression that resolves to one function (an
  identifier or a module-qualified identifier) and is **not** type-checked as a value (a bare function
  name used as a value is rejected today). `input:` is the argument for the target's single `consuming`
  parameter and, as for any consuming argument (§4.3), **must be written with `consume`**, a literal or
  temporary included; a missing marker is `type-checking.invalid-consume-argument`.
- The result is `JoinHandle<R>`, `R` the target's result type, `Void` included.
- `JoinHandle<R>` is a builtin type with one type argument, usable in any type position subject to the
  linear rules of 0015. A wrong number of type arguments is the existing generic-arity diagnostic.
- `consume handle.join()` is its only member. Without `consume`, or on a borrowed or `inout` handle, it
  is `invalid-consume-argument`. Passing the handle to a `borrowing` parameter is allowed; only `join`
  ends it.

### Target rules

The target must be a **top-level function** that is not `@c_extern`/`@c_export`, not `main`, has
**exactly one parameter, `consuming`**, and does not return `Never`. "Not generic" holds trivially:
user generic functions do not exist yet (§3.2.3), so that case has no test until they do. Two
functions of one name cannot coexist in a scope (name resolution reports a duplicate declaration), so there is no
overload case. A public function of a source dependency is a valid target; a
private one is not visible (§12). `let f = square; spawn(function: f, ...)` is not a function name
(§20.1).

A zero-parameter function is rejected (0013 Q9) so `spawn`'s shape does not depend on its target. The
help line says: wrap the input in a struct, or pass a unit-like struct. A parameter of an empty struct
type is accepted and occupies no bytes in the block.

The rule is a subset of a future `function:` argument of the §2.3 function type provided `spawn` is
later redeclared as a generic builtin: every program it accepts stays valid.

### Static semantics

- `JoinHandle<R>` is linear (0015): a path that leaves one unjoined is
  `semantic-analysis.linear-not-consumed` (its help names `join`); `linear-overwrite`,
  `linear-result-dropped` (a `spawn(...)` statement), the collection rules and
  `type-checking.copy-of-noncopyable` apply as 0015 specifies. `R` may be linear (a handle can be
  returned from a task and joined elsewhere) and a handle can be an `input`. `join` ends a handle in a
  struct field field by field (`consume w.handle.join()`); a struct holding one cannot declare `deinit`.
- A `?` between `spawn` and `join` leaves the handle live and is `linear-not-consumed`; the borrowing
  idiom of 0015 applies. `(consume h.join())?` is fine when `R` is a `Result`.
- **A handle moved into a call is pending until the call commits** (0015 rule 3, implemented here
  because `JoinHandle` makes it reachable): in `take(handle: consume spawn(...), extra: f()?)`, or an
  initializer `Holder(handle: spawn(...), extra: f()?)`, the `?` runs after the task has started and
  would drop the handle on its error path, so it is `semantic-analysis.linear-not-consumed` at the `?`,
  and the help says to evaluate the `?` first. The first review found this program compiling and dying
  at run time with `task dropped without join`; the trap stays as defence in depth. Array and
  dictionary literals and enum payloads are not covered yet.
- An input that is a partial move (`consume pair.first`) follows the ordinary partial-move rules; a
  `deinit` type cannot be partially moved.
- Name resolution: `spawn` and `JoinHandle` are in the prelude scope; see option 2.
- A `spawn` inside a `deinit` is allowed (the exit check runs on `deinit` bodies). It is join-on-drop in
  disguise, which 0013 Q18 rejected as hazardous, so the spec says the deadlock rules apply.

### Dynamic semantics

- **Spawn** evaluates the arguments, allocates the task block, zeroes the thread record, moves the input
  into the block, and starts a thread through `joyeer_thread_start_abi` whose entry is the target's
  thunk. Starting the first thread turns the runtime locks on (0017). If the runtime refuses the thread
  (code `-1073741824`, the 256 limit, 0016, or the operating system), the program **traps** with
  `too many tasks` or `cannot start task`. This is deliberate (Rust's `thread::spawn` also panics): the
  alternative, a `Result` that carries the unspent input back as `SendError` does, is a later
  `trySpawn`, not v1.
- **The task** loads the input, calls the target, stores the result in the block, and returns. The
  target's locals are destroyed by the target before the thread ends, so they are destroyed before
  `join` returns.
- **Join** calls `joyeer_thread_join_abi`, which on failure traps with `cannot join task`. Then it moves
  the result out, frees the block with its stored size, and returns the result.
- **Order:** spawn before the task's first statement; the end of the task, including the destruction of
  its locals, before the `join` that returns (§21.4). A `deinit` runs on the thread that owns the value
  when it is destroyed: a moved input's on the task, a joined result's on the joiner.
- **A trap in any task** exits the process (0017), with no destructor; a joiner blocked in `join` is
  simply ended by the exit.
- **Stack overflow in a task** is not trapped: a task's 8 MiB stack ends in the operating system's fault
  handling. The main thread differs (8 MiB `ulimit` on Linux, 1 MiB on Windows).
- **Exit:** the linear check makes every handle joined; the entry's unjoined-thread check (0017) is the
  backstop.

### IR and lowering

- **IR.** `task_spawn(function, input) -> task<R>` and `task_join(task<R>) -> R`; `task<R>` is a new IR
  type that records only the result type. Both operations carry the source span and debug scope. The
  verifier checks `task_spawn`: the function is a top-level, non-extern, non-`main` function with one
  consuming parameter whose type is the input's type and whose result is `R`.
  The IR wire format gains the type and operations (version bump).
- **The block layout.** `[128-byte thread record, zeroed][8-byte total block size][result][input]`,
  where the result starts at `roundup(136, align(R))` and the input at the next multiple of its own
  alignment (at most 16 each). `join` therefore finds the result from `R` alone and frees the block
  with the stored size (the allocator has no header, so `joyeer_free` needs the exact size). The 128 is
  `JOYEER_THREAD_BYTES`, taken from one shared definition in the compiler, with a test that it equals the
  runtime header's value. A `Void` result and a zero-size input occupy no bytes.
- **The thunk** is one internal LLVM function `void(ptr block)` per distinct target, created by the backend
  with the first `task_spawn` that names the target. It loads the input from the block (consuming
  parameters are passed by value in this compiler), calls the target exactly as a direct call does, and
  stores the result at the result offset. It has no debug scope of its own and is `noinline`, so it
  cannot corrupt a caller's line tables. The runtime trampoline calls it with one pointer.
- **Destroy helper.** The generated destroy helper for a `task<R>` leaf (reached through an
  `Optional`, an array, or a struct field that the linear check did not cover) **traps** with
  `task dropped without join`: the defence in depth deferred in 0015. (Lowering emits an ordinary
  scope-end `destroy` of every handle slot, so the verifier cannot reject a `destroy` of `task<R>`; the
  helper is a no-op on a joined or moved-from handle because both are zero.)
- **Link.** The program object references the thread functions, so the thread archive member is linked
  (lazy on Linux and the Windows own-entry link). The compiler computes `usesThreads` from the IR (any
  `task_spawn`) and passes it in its own `LinkOptions`; the backend C ABI is unchanged because `-lpthread`
  is one more linker argument, and only the IR wire version changes. On Linux a task program is linked
  with `-lpthread` after the
  archive group, always (an empty library on glibc 2.34 and later and on musl; 0016 documented 2.34 as
  the minimum without it). macOS needs nothing. The import gate for a task program allows exactly the
  thread object's per-platform imports; every other program's gate is unchanged.
- **`runProcess`** (0013 Q13, kept): `semantic-analysis.run-process-with-tasks` is reported at a call to
  System's `runProcess` when the compiled program (root plus every source dependency) also contains a
  call to the prelude `spawn`. "System's" means a function named `runProcess` in the module named `System`
  (the semantic model records module names). The definition is textual, not reachability, and a user function that
  happens to be named `runProcess` does not count. It is **not modular**: a library that calls
  `runProcess` (Fern's executor) becomes uncompilable once a consumer adds a `spawn`, and the error is
  reported inside code its author may not own, so the diagnostic cites one `spawn`. The 0013 rationale
  has weakened since 0017: the Linux child now allocates nothing before `execve` and Windows and the
  POSIX `posix_spawn` path never had the hazard. The rule stays because the user accepted it; lifting it
  is recorded as an open question below.

### What the compiler must grow

- A **builtin type kind** for `JoinHandle<R>` (type interning, equality, copyability, linearity, layout,
  copy/destroy helpers, IR and backend types).
- A **consuming builtin member**: `receiverEffectOf` returns only `inout` or `borrowing` for a builtin
  member, semantic analysis honours a consuming receiver only for user methods, and lowering keys on the
  member's name. The signature's `receiver->access` already exists and is used instead of a flag.
  Builtins are dispatched by name strings today (`removeLast`, `Dict.get`); with `spawn` and `join`
  that becomes five, so the work introduces a `BuiltinOp` enum on the symbol and moves them onto it.
- A **builtin function with a computed result**: no prelude function derives its result from a function
  name argument today, and the `function:` argument bypasses expression checking.
- **`Array.consumeEmpty()`** (stage 0, below).

### Stage 0: `Array.consumeEmpty()`

Needed so an array of handles can end at all (0015 lists it as not yet built); without it a fan-out of
tasks is `linear-not-consumed` and a handle declared in a loop body must end in the iteration. It is a
consuming member of `Array<T>`: if the array is not empty it traps (`array not empty`) before any side
effect; otherwise it frees the storage and the array is ended. It is the only way to end an array of
linear elements. Specified as §2.6.4, with an IR operation, tests and a spec example. It depends on the
consuming builtin member above, so the two are built together.

## Diagnostics (stable IDs)

`type-checking.`: `spawn-target-not-function` (not a name, or a variable; an unknown name is name
resolution's), `spawn-target-unsupported` (a method, `@c_extern`/`@c_export` or `main`; the message names
which), `spawn-target-parameters` (not exactly one parameter, or not `consuming`),
`spawn-target-never`. There is no overload diagnostic: two functions of one name are already
`name-resolution.duplicate-declaration`. Reused: `invalid-consume-argument`,
`copy-of-noncopyable`, the generic-arity diagnostic, `linear-collection-element`,
`linear-top-level-binding`, `linear-in-deinit-type`. `semantic-analysis.`: `run-process-with-tasks`,
`use-after-consume`, `linear-not-consumed`, `linear-overwrite`, `linear-result-dropped`.

## Not in this decision

Channels (0019); a `select` or try-receive (0013 Q11); structured scopes, actors, async; names,
priorities, cancellation, affinity and stack size; `trySpawn`.

**`System`'s first-use state**, assigned to P5 by 0017, is audited: Winsock is started on every socket
creation and is reference counted by the operating system (`network_windows.joyeer`), and the only
shared slot `System` touches is a read of slot 16, written once before `main` (`cruntime.joyeer`). No
guard is needed, so §21 may say a task can use `System`.

## Consequences

- A reviewer reads from a signature what crosses a thread: the `consuming` parameter and the result.
- A task costs one heap block, one thread start, and one thunk per target; a thread per task is not
  cheap, so P7 uses a worker pool.
- Every task needs a join on every path, clumsy around `?`; the borrowing idiom applies.
- The compiler gains one builtin type kind, one consuming builtin member, one computed builtin
  function, the `BuiltinOp` refactor, two IR operations and one IR type, a thunk, a link option, and a
  whole-program rule.

## Tests

- Per stage, citing the spec: parser accepts the labeled call; name resolution (a same-module, a local
  and an imported `spawn`, a user `struct JoinHandle`); one type-checking fixture per diagnostic
  (generic is unreachable); semantic analysis for every linear rule with real handles, including a task
  returning a handle and a handle as an input; IR lowering and the verifier; the backend object.
- End to end: spawn and join for Int, String, array, a struct with `deinit`, `Void`, `Result`;
  **fan-out and fan-in** over an array of handles (spawn N, drain with `removeLast` in a `match`, end
  with `consumeEmpty`); a heap input is moved, not cloned, and a constant array as input **or result**
  is cloned, never freed (allocation counter); locals destroyed before `join` returns; a result
  visible after join; many tasks; tasks that spawn tasks; a self-spawning target with no joins hits the
  limit; a trap in a task exits once with status 1 and no destructor, including while `main` is blocked
  in `join`; two simultaneous traps; two tasks printing; a task that allocates and a joiner that frees.
- Backend: block size and result offset for differing input and result sizes and alignments; the
  destroy-helper trap; thunk dedupe per target; a debug subprogram under `-gfull`; the import gate;
  hello-world size unchanged; `-lpthread` on Linux; the wire and backend ABI versions; the
  `JOYEER_THREAD_BYTES` match.
- `runProcess`: the rule fires with a `spawn` in any module, not without one, and not for a user
  function of that name.

## Open questions

1. **Lift the `runProcess` ban.** The hazard 0013 Q13 named is gone on every platform; the ban blocks
   Fern from ever running its executor in parallel. Recommended: reconsider after P5a, with Fern as the
   evidence. It needs the user's decision because Q13 was accepted.
2. **`trySpawn`.** A recoverable spawn that returns the unspent input. Recommended: when a server needs
   it, not before.
3. **Whether a task may be started from a `deinit`.** Allowed above; revisit if it deadlocks a real
   program.

## Evidence

- 0013 Q1, Q2, Q3, Q9, Q13, Q15, Q18; 0015 "Linear types (P2c)"; 0016 "Gating and the link"; 0017.
- The review's checks against `typechecking.cpp`, `semanticanalysis.cpp`, `allocator.joyeer` and
  `threads.joyeer`: the block layout must let `join` find the result and the free size from `R` alone,
  the consuming-argument marker is mandatory, and builtin members cannot consume a receiver today.
- Rust's `thread::spawn` takes a closure and returns a `JoinHandle<T>` and panics on failure; scoped
  threads tie the lifetime. Joyeer has neither closures nor lifetimes, so it uses a named function and a
  moved argument.

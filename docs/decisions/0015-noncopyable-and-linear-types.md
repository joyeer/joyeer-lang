# 0015: Noncopyable and linear types, and handles that close

- Status: accepted (2026-11-10), implemented. Phase P2 of the
  [concurrency plan](../plan/0009-concurrency.md). `deinit`, noncopyable types, owning pattern
  bindings and `Array.removeLast` are implemented (P2a, P2b); linear types (P2c) are specified
  below; their checks are implemented (see "Implemented as" under the stages) except as noted.
  The one fork in it, how a handle closes, was settled the same day in favor of a
  real `deinit` ("The fork"); that revises the answer accepted in
  [0013](0013-concurrency-model.md) question 6, which now says so.
- Spec: none yet. When accepted it amends [§4.1.1](../../specs/04-memory.md) (copyability),
  [§4.7](../../specs/04-memory.md) (destruction) and [§2.6](../../specs/02-types.md) (builtin
  generics), and [§18.6](../../specs/18-host.md#186-tcp-networking) (handles).

## Context

Concurrency (0013) moves values between threads. That is only trustworthy if the
values that denote an owned resource, a socket, a task, a channel end, cannot be
duplicated, cannot be silently dropped when they must be finished, and release their
resource when they go away. The language does not provide any of that today.

What the compiler does today, found by reading it:

- **Copies are implicit and untyped.** Nothing at the type level says a type is
  copyable. A copy is created during IR lowering: `prepareOwned` in
  `compiler/ir/irlowering.cpp` emits a `copyValue` whenever a borrowed value is used
  where an owned one is needed. The spec's rule that a type with a `deinit` is
  noncopyable (§4.1.1) has no implementation, and the compiler has no `deinit`.
- **Consumption is tracked, but only one way.** `InitializationBuilder` in
  `compiler/frontend/semanticanalysis.cpp` keeps, per program point, the sets
  `initialized`, `possiblyInitialized` and `consumed`, plus consumed projections,
  merges them at branches and loops, and reports `useAfterConsume`. It does not
  check the inverse: that a value *is* consumed before its scope ends.
- **Destruction is generated per type.** `destruction->requiresDestruction(type)`
  decides, and the backend (`compiler/backend/native_ownership.cpp`) emits a destroy
  helper for each type. A type-specific action at destroy time has a natural place.
- **Handles are plain structs.** `TcpConnection` and `TcpListener` hold an `Int`
  and no `deinit`, so they are copyable, and destroying one does not close it.
  A `match` pattern binding is not an owning local, so a received handle cannot be
  `consume`d (0012, open question 1).
- **Arrays cannot give elements back.** `Array.removeLast` and a consuming element
  accessor (§4.5.1) do not exist, so a program cannot take a task or a connection out
  of an array it owns.

## What is needed

1. **Noncopyable types.** A copy of one is a compile-time error, and the property
   propagates (§4.1.1) to arrays, optionals, results, structs and enums that contain
   one, so `Result<TcpConnection, NetworkError>` becomes noncopyable too. The `Copyable`
   builtin of §20.5 must be false for them.
2. **Linear types.** A subset of noncopyable types that must be consumed on every
   path: leaving a scope with one still initialized is an error, and the generated
   destroy helpers must never silently drop one (an array of tasks must be taken
   apart, not destroyed). Needed for `JoinHandle` (0013, question 18).
3. **Handles that close.** Destroying a noncopyable handle, for example a connection
   queued in a channel that is dropped, must release the operating-system resource,
   or the descriptor leaks.
4. **Owning a received value.** Consuming a value bound by a `match` arm, or another
   way to own what a channel or `tcpAccept` returned.
5. **Taking elements out of an array.** At minimum `removeLast`; ideally a consuming
   element accessor.

## The fork: how a handle closes (item 3)

0013 question 6 accepted "`deinit` is not required first; give the decreed handle
types a compiler-generated destroy helper that closes the resource". Building that
shows two real options, and a third that was already rejected.

1. **A restricted resource annotation.** Something like `@resource(close: closeFn)`
   on a struct, usable only in modules that already hold `native-bindings`
   authorization, which marks it noncopyable and makes the generated destroy helper
   call `closeFn`. Small surface, no new general feature. But it is a second,
   special mechanism: a one-off cousin of `deinit` that ordinary programs cannot use,
   and it duplicates what §4.7 already specifies.
2. **Implement `deinit` as §4.1.1 and §4.7 already describe it.** A `struct` with a
   `deinit` body is noncopyable and the compiler calls the body when the value is
   destroyed. `TcpConnection` becomes an ordinary struct with `deinit { close }`.
   One mechanism, in the spec already, usable by any program with a resource. Cost:
   parser, name resolution, type checking (the body's `self`), semantic analysis,
   lowering of the call, and the destroy helper calling user code, all new. Larger
   than option 1, but it is work the language owes anyway.
3. **A closing builtin type** (the handle is a compiler-known type with no source
   declaration). Rejected: `System` would have to name an opaque builtin, and it
   gives every future resource its own compiler change.

**Settled (2026-11-08): option 2.** It keeps one way to do one thing (preamble
principle 4), needs no authorization special case, and the other items in this
decision (noncopyable propagation, destruction order) are needed for `deinit`
regardless. This revises 0013 question 6, which had said `deinit` was not required
first. Option 1 is not built, so there is no stopgap to retire later.

Linear types for `JoinHandle` and the channel ends do not need either option: they are
compiler-known builtins with runtime-generated destroy helpers, as `Array` and `Dict`
are today.

## Decision

Handles close through a real `deinit`, as §3.7, §4.1.1 and §4.7 already specify, and
the rest as follows.

- **`deinit` (v1 scope).** `deinit() { ... }` is allowed in the body of a `struct`
  (not in an extension, and not in an `enum` in v1), at most once, with no parameters,
  no visibility modifier and no `mutating` or `consuming`. Its receiver is the value
  being destroyed, read through `self.field` (implicit-`self` field access is not
  implemented, §3.9). The body may not consume `self` or move a field out of it, may
  not call `deinit` explicitly, and runs to a `return` or the end of the block. The
  compiler runs it when the value is destroyed (§4.7) and then destroys the stored
  fields in reverse declaration order. A trap inside it ends the process like any trap.
  A struct that declares `deinit` is noncopyable, and so is anything that contains one.
  The `init_decl` of §3.7 stays unimplemented: a struct with a `deinit` is built with
  its memberwise initializer, from inside its module.
- **Copyability is a property of a type** that the type checker can query, with the
  recorded reason (`deinit`, a noncopyable field, a builtin). A copy site is an
  implicit duplication of a noncopyable value; the check belongs where the expression
  and its context are known (type checking or semantic analysis), not only in lowering,
  so the diagnostic is precise. `prepareOwned` keeps an assertion as a backstop.
- **Linearity** applies to `JoinHandle` and what holds one, and propagates through containers like
  noncopyability. The initialization analysis gains the inverse check, "possibly live at an
  exit", specified under "Linear types (P2c)" (a plain "consumed" check over the existing merge
  would accept a leak on one branch). `?` and early `return` are exits. A destroy helper for an
  aggregate containing a linear value is a compile-time error rather than a drop.
- **Consuming a pattern binding.** When the scrutinee of a `match` is an owned
  temporary, an arm's payload binding is an owning local, so `consume` on it is
  allowed. This is the fix for 0012's question 1 and removes the `?`-only workaround
  from the web server.
- **`Array` gets `removeLast`** returning the element as an owned value, and the
  consuming element accessor specified in §4.5.1 is implemented (or explicitly
  deferred in favour of `removeLast` plus a loop).
- **`System` handles** become noncopyable and closing through `deinit`:
  `TcpConnection` and `TcpListener` each gain a `deinit` that closes the socket if it
  is still open. `tcpCloseConnection`/`tcpCloseListener` keep their explicit,
  error-reporting form: they close the socket, report the result, and store an invalid
  handle value (`-1`) in the consumed value so its `deinit` finds nothing to close. A
  handle dropped without an explicit close (a queued connection, or an error path) is
  closed by `deinit`, whose result cannot be reported. Their error-carrying results
  become noncopyable by propagation, and the tests and the web-server example are
  updated.

## Implementation outline (the `deinit` feature, stage by stage)

The repository's feature workflow applies: spec, failing tests, then each stage, then
role reviews and validation. Progress: stages 1 to 8 are implemented and reviewed (Linux and Windows x64 Debug and Release pass);
what remains of stage 9 is the full `scripts/validate.py` acceptance run (Release as well). Stages, with what each must do:

1. **Spec.** Amend §3.7 (the rules above, with an implementation-status note),
   §4.1.1 (propagation and `Copyable`), §4.7 (order with partial moves forbidden) and
   §18.6 (handles).
2. **Lexer and parser.** `deinit` is already a keyword (`token.cpp`). Parse
   `deinit() { ... }` inside a struct body into a new syntax node with a span; reject it
   in an `enum`, an `extension`, with parameters, with a visibility, or twice, with
   parser diagnostics.
3. **Name resolution.** Give the body a scope in which `self` names the receiver, as
   for a method; a single `deinit` symbol per struct that cannot be named or called.
4. **Type checking.** Type the body with `self` as a borrowed receiver; reject
   consuming `self` and moving a field out of it; record on the struct's type whether
   it has a `deinit`, and compute noncopyability and its propagation to containers and
   aggregates; answer `Copyable` (§20.5) from that. Report a copy of a noncopyable
   value at its copy site. *Implemented as* `TypeCheckedModel::isCopyable` and
   `noncopyableReason`, plus `type-checking.copy-of-noncopyable`, reported in the type
   checker at the owned contexts: a binding initializer, a plain assignment, a `return`
   (an owning local or consuming parameter is moved, anything else is a copy), a
   memberwise-initializer or enum-case argument, `append`, and array and dictionary
   literal elements. Only a place (a local, a parameter, a stored field, a subscript)
   copies; a call result or constructed value is a temporary. Cases still to add:
   the `?` operand and a `match` scrutinee. `Copyable` as a generic bound waits on §20.
5. **Semantic analysis.** Run the existing initialization and consumption analysis over
   the `deinit` body; reject use of a struct with a `deinit` in a partially consumed
   state (a field moved out), since the destructor needs the whole value. *Implemented
   as* `semantic-analysis.partial-consume-of-deinit-type`, reported when a consumed
   field or array element has a `deinit` type as the base of its path (a wrapper without
   a `deinit` may still give up a handle field), and `forEachFunction` visits the
   `deinit` body. `TypeCheckedModel::deinitializerOf` answers which struct has one.
6. **IR and lowering.** Lower `deinit` to an internal function taking the value's
   address; make `requiresDestruction` true for such a type; make destroy emit the
   `deinit` call before field destruction, once per destruction, on overwrite and at
   scope end. Keep `prepareOwned`'s copy path as an assertion for noncopyable types.
   *Implemented as* `ir::StructureDefinition::deinitializer` (the lowered function, named
   `Type.deinit`, taking the address) and `DestructionAnalysis` treating such a struct as
   needing destruction; lowering rebuilds its analysis once the `deinit` is linked.
   Moved-from storage must not run the `deinit`, and `take` zero-fills, so the struct gets a
   hidden trailing `$live` Bool field that construction sets and zeroing clears; this also
   makes a destroyed or uninitialized value inert. A `return` of an owning local of a
   noncopyable type is lowered as `take`; `prepareOwned` reports `ownership-violation`
   instead of emitting a copy, and a `match` binding of a noncopyable payload is rejected
   (`a pattern cannot bind a noncopyable value yet`, until P2b).
7. **Backend.** The generated destroy helper for the type calls the `deinit` function,
   then destroys the fields. Source and debug scopes of the body are preserved.
   *Implemented as* a branch on the live flag in the generated destroy helper that calls the
   deinit function, then the existing reverse-order field destruction; helper bodies are
   emitted after all source functions are declared. The IR wire version is 11 for the `deinit` change and 12 once the `array_remove_last` and
   `payload_address` opcodes were added.
8. **System and the example.** Add `deinit` to the handle structs, adjust their close
   operations, and update the tests and the web server.
   *Implemented as* `deinit` on `TcpListener` and `TcpConnection` (`var handle`, closed by
   `discard` unless it is -1); `tcpCloseConnection` and `tcpCloseListener` still close
   explicitly, to report the result, and store -1 first so the destroy of the consumed
   parameter does nothing. The tests add dropped-handle cases (a dropped listener frees its
   port, a dropped client ends the stream), and the web server needed no change.
9. **Docs and acceptance.** Update `supported-features.md`, the portable skill copy, and
   the decision records; run the role reviewers and `scripts/validate.py`.

Compiler changes are built and tested on Linux in WSL as well as on Windows. A change to
C++ needs a compiler rebuild; the Windows build needs an MSVC Developer shell, which is
the user's to run.

## Consequences

- Easier: a reviewer can see from a type that a value is a resource; double close and
  descriptor sharing across threads stop being possible; the concurrency types in 0013
  have the footing they need.
- Harder: this is a language feature through every stage, and it changes common code:
  any `Result<TcpConnection, ...>` is now noncopyable, so places that relied on copying
  one (including a few in the tests and `System` itself) must change. An audit is part
  of the work.
- Harder: linearity makes early exits explicit. A function that spawns a task and then
  returns on an error must join it on that path. There is no `defer` (reserved, Appendix A);
  whether the language needs one is the open question below.
- Tests, per the feature workflow: failing fixtures at each stage and GoogleTests
  citing the spec sections, covering: copying a noncopyable value (assignment,
  argument, return, capture in an aggregate, array element) is rejected; propagation
  to `Array`, `Optional`, `Result`, struct and enum; `Copyable` is false; a linear value
  left unconsumed at the end of a function, block, loop iteration, `if` branch and `match`
  arm is rejected, and consumed on every path is accepted; assigning over a live linear
  value is rejected; an array of linear values cannot be dropped; destroying a handle
  closes it exactly once (allocation and descriptor counters); a consumed pattern binding
  works and double use is `useAfterConsume`; `removeLast` returns ownership.
- New diagnostics (stable IDs): copy of a noncopyable type, linear value not consumed,
  linear value dropped by an aggregate or container, consuming a non-owning binding.

## Open questions

1. **`deinit` on an `enum`, and the `init` declaration.** Both are in the spec grammar
   and out of v1 here: `deinit` is struct-only, and `init` stays unimplemented.
   Whether either is needed for a later resource type is decided when one appears.
2. **`defer`.** Do early exits with linear values make a scope-exit statement necessary
   (the reserved `defer`), or is explicit joining on each path acceptable?
3. **Where to diagnose a copy.** Type checking (needs a notion of copy sites there) or
   semantic analysis (has flow state but less type context). Decide while writing the
   failing tests.
4. **Partial consumption of a struct that contains a linear field.** Allowed through
   the existing projection tracking, or rejected in v1?
5. **`removeLast` versus a consuming accessor** as the first array operation.
6. **Mutating a consumed value to defuse its `deinit`.** Settled by implementation: a
   consuming parameter is mutable, so a close that reports its result closes the handle,
   writes `-1`, and lets the parameter be destroyed; the `deinit` skips `-1`. A move needs no
   sentinel, because `take` clears the hidden live flag in the source.
7. **Moving a noncopyable value out through a trailing expression.** `return x` is a move;
   a block or function whose last expression is a noncopyable local is still lowered as a
   copy and is refused by `prepareOwned`. Decide whether to treat it as a move or require
   `return`.
   Settled for now: type checking reports it as a copy (`copy-of-noncopyable`) and the
   author writes `return`.
8. **When a temporary is destroyed.** A discarded call result, or a temporary read for one
   field, lives to the end of its enclosing block, so a `TcpConnection` temporary holds its
   descriptor until then. The spec does not define statement-level lifetime. Decide whether
   temporaries should die at the end of their statement before handles are built from them.

## Owning pattern bindings and Array.removeLast (P2b)

Two gaps stopped a program from owning a handle it received. A `match` binding copies its
payload, and an `Array` could not give an element back.

- **`Array<T>.removeLast()`** (spec 2.6.2) is a mutating builtin returning `Optional<T>`; the
  element moves into the result. A consuming element accessor (open question 5) is not needed
  for the first use and stays deferred. IR `array_remove_last`; runtime
  `joyeer_array_remove_last_abi`.
- **A pattern that binds a noncopyable payload moves it** (spec 7.1.2). The scrutinee must be an
  owned temporary or `match consume x`; matching a place without `consume` is
  `copy-of-noncopyable`. Lowering stores the scrutinee, addresses the payload (`payload_address`),
  `take`s it into the binding, which owns it for the arm. Unbound payloads stay in the storage and
  are destroyed with it. This resolves 0012's open question 1: the worker loop can bind a received
  connection without `?`.

## Linear types (P2c)

0013 wanted `JoinHandle` to be consumed on every path. This section settles what that means, how
the checker enforces it, and how an aggregate ends. It was reviewed by the language designer, and
the first draft was cut down: it had a user-visible `linear struct`, a `discard` builtin, linear
channel ends, and an exit check that accepted a leak on one branch. What follows is the smallest
design that makes `JoinHandle` safe.

### Definition

- A type is **linear** when it is the builtin `JoinHandle<R>`, or holds a linear type: a stored
  field, an enum payload, or the element, payload or error type of `Array`, `Optional` and
  `Result`. Linearity propagates exactly as noncopyability does (§4.1.1) and every linear type is
  noncopyable. `Copyable` is false for it.
- **Only `JoinHandle` is linear.** `ChannelSender<T>` and `ChannelReceiver<T>` are noncopyable
  and close in their destroy helper, as `TcpConnection` closes through `deinit`. This **replaces**
  0013 resolution 18, which made the ends linear: a linear end bought no memory or deadlock
  safety (the runtime block is freed by the last end, and the checker cannot order a `close`
  before a `join`) and put two or three closes on every `?` of worker code. The explicit
  consuming `close` stays for readability.
- A linear value is **never destroyed implicitly**. Where §4.7 destroys an owning binding at scope
  end, a linear binding must already be ended, or the program does not compile.
- **Transfer and end.** Consuming a value moves it (`consume x`, a `consuming` argument, a
  return, an initializer, an enum payload, `append`, a binding, a match that moves a payload): the
  receiver takes on the obligation. A linear value **ends** only in a builtin consuming operation:
  `join` for `JoinHandle` (P5), and, for an `Array` of linear elements, `consumeEmpty()`, which
  traps before any side effect if the array is not empty; `removeLast` (spec 2.6.2) drains it, last
  element first.
- **A linear aggregate ends field by field.** A struct or enum that holds a linear type has no
  `deinit` (below), so its fields may be moved out of it (partial consumption, already
  implemented for wrapper structs). The owner must consume every linear field path; the
  remaining fields are destroyed as usual at scope end. A linear aggregate is **live** while any
  of its linear field paths is possibly unconsumed.
- **No `deinit` with a linear field.** A struct that is linear by propagation may not declare
  `deinit`, and a struct with `deinit` may not hold a linear field: its destruction would drop the
  field, and a `deinit` body cannot move a field out (§3.7.1). The exit check also runs on
  `deinit` bodies.
- **Not allowed in v1:** a linear key or value in `Dict` (it cannot be drained); `for x in` over an
  array of linear elements (no consuming iteration); a linear element type for
  `ChannelSender<T>`, `ChannelReceiver<T>` and `SendError<T>` (a queued value would be dropped
  when the channel closes); a linear type as the type argument of a user type parameter (§20.5
  makes parameters droppable); a top-level `let` of linear type; overwriting an array element
  (`&a[i] = h`), because element consumption is not tracked.

### The check: "possibly live", not "consumed"

`InitializationBuilder` merges `consumed` as a union (may-consumed), so a check phrased "must be
consumed at exit" would accept `if c { join(consume h) }` followed by an exit, and leak on the
else path. The property is the inverse: a linear binding is **live** at a point when it is in
`possiblyInitialized`; for an aggregate, live also when any linear field path is not *definitely*
consumed, which needs a must-consumed intersection over projections (new). At every **exit** of
the binding's scope a linear binding must not be live. An exit is: the end of a block, an early
`return`, `?` propagation, `break` and `continue` out of the loop body that contains the binding's
scope, and the end of a `match` arm for a binding the arm introduced. `forgetBinding`, where scope
exit drops facts today, is the hook.

1. **Early exit with a live linear binding** is `semantic-analysis.linear-not-consumed`, reported
   at the exit, naming the binding and its creation site. A `Never` call or a trap needs no end,
   because the process exits without cleanup.
2. **Assignment over a live linear value** (a local or a field path) is
   `semantic-analysis.linear-overwrite`. Assignment into an ended or uninitialized place
   reinitialises it and is allowed. `&a[i] = v` over a linear element is always `linear-overwrite`.
3. **A consume takes effect when its call or construction commits.** In `f(a: consume h, b: g()?)`,
   `[consume h, g()?]` or `W(task: consume h, x: g()?)`, the `?` runs while `h` is still live, so
   that exit reports `linear-not-consumed`. (Lowering's pending-operand cleanup would otherwise
   drop it.)
4. **An owned linear temporary must be moved immediately.** It may appear only as a binding
   initializer, the right-hand side of an assignment to an ended or uninitialized place, a
   `return` value, a consuming argument, an aggregate or enum component, an `append`, or a match
   scrutinee. Anywhere else (a statement `spawn(...)`, `f(g())` with a borrowing `f`, `g().m()`,
   a field read through one) is `semantic-analysis.linear-result-dropped`. `_` ignores nothing: a
   `.Ok(_)` or `.Some(_)` over a linear payload of a moved scrutinee, and a bare `_` arm over one,
   are `linear-result-dropped`. (`let _ = e` is not valid syntax, so it needs no rule.)
5. **`match`.** A pattern over a linear scrutinee must bind every linear payload (a payload no arm
   binds would be destroyed with the scrutinee, which is a drop). Spec 7.1.2's "stay and are
   destroyed" sentence applies to non-linear payloads only. `match consume x` ends `x` once its
   payloads are bound.
6. **`?`, `!`, `??`, `?.`** on a linear `Optional` or `Result`: `?` and `!` consume an owned
   operand (a temporary or `consume x`), and a borrowed operand is `copy-of-noncopyable`. `??` and
   `?.` are rejected on a linear operand in v1; use `match`.
7. **Loops.** A linear binding declared in a loop body must be ended by the end of the iteration.
   One declared outside is ended in the body only if the body reinitialises it before the back
   edge; the existing merge reports the rest as use-after-consume. Shadowing is tracked by symbol.
8. **An `Optional<L>` local is live even when it holds `.None`.** The idiom to refill one is
   `match consume slot { .None => (), .Some(h) => ... }`, then assign `slot` again.

### The borrowing idiom, and `defer`

`defer` is not added; it stays reserved. A function that only borrows or takes `inout` a linear
aggregate has no owning binding, so the exit check does not apply to it, and `?` inside it needs
no joins:

```joyeer
func run(pool: inout Pool): Result<Void, NetworkError> { ... freely uses ? ... }

var pool = startPool()
let result = run(pool: &pool)
consume pool.shutdown()     // ends every handle once
```

The diagnostic's help names this idiom. `defer` is re-decided after the P7 worker-pool prototype,
if a function needs more than one cleanup helper. The check is compatible with it: a deferred
consume would end the binding on every exit.

### Lowering and the backend

A program that passes the check ends every linear value, so no destroy of one is reachable and
the ordinary scope-end cleanup (a destroy of storage that has been zero-filled by `take`) does
nothing for it. Defence in depth is a later stage: an IR `linear` bit on the type, no scope-end
cleanup for linear bindings, a verifier rule that rejects a destroy of a linear type, and a wire
version bump; `consumeEmpty()` frees only storage. The runtime allocation balance backstops a
checker bug.

### Test seam and what waits for threads

`JoinHandle` does not exist until P5, but the rules do not depend on how a type becomes linear.
For tests, the type checker's `isLinear` also honours a `linear` modifier on a struct declared in
a **toolchain-authorized** source (a runtime or core unit, the same authorization as native
bindings); anywhere else the modifier is `type-checking.linear-reserved`. It is not public
syntax. A public `linear struct` waits for a second use case, such as a user type that must be
committed or rolled back, or `JoinHandle<R>` written in Joyeer once user generics exist. Waiting
for threads: the `JoinHandle` and channel runtime, `join`, the channel-element fixtures, the
close-before-join deadlock tests, and the worker-pool trial.

### Diagnostics

`semantic-analysis.linear-not-consumed`, `linear-overwrite`, `linear-result-dropped`;
`type-checking.linear-in-deinit-type`, `linear-collection-element` (Dict key or value, `for-in`),
`linear-top-level-binding`, `linear-struct-without-fields`, `linear-channel-element`,
`linear-type-argument`, `linear-reserved`. 0013 and the plan name
`task-not-joined` and `Task<R>`: they become `linear-not-consumed` and `JoinHandle<R>`.

### Stages (P2c)

**Implemented as (stages 2 to 4):** the contextual `linear` modifier before `struct`
(parser; `StructDeclSyntax::isLinear`), reserved for runtime and core sources
(`type-checking.linear-reserved`); `TypeCheckedModel::isLinear`, `linearLeafPaths` and
`isDeclaredLinear`; the type-level rules `linear-in-deinit-type` and `linear-collection-element`
(a `Dict` key or value, `for-in` over an array, a top-level constant); and in semantic
analysis `linear-not-consumed` at the end of a block, a match arm, a function, a `return`, the
failure path of `?`, and `break`/`continue`, with a must-consume intersection over projections,
`linear-overwrite` for a whole binding, a field path or an array element, and
`linear-result-dropped` for a statement value (including a block, `if`, `while` or `match` tail
that is not used as a value), an owned temporary that is only borrowed (a borrowing argument, a
field read, a borrowing receiver), and a `_`, `.Some(_)`, `.Ok(_)` or bare `_` arm over a linear
payload of a moved scrutinee. Peeking at a borrowed scrutinee with `_` is fine. The join of a
whole consume on one path with per-field consumes on the other counts each field as consumed
(coverage, not set equality), and an aggregate can be refilled once every leaf was consumed. A struct
declared `linear` ends field by field like any aggregate, with every field a leaf (copyable ones
included, destructuring), because there is no `discard`; a struct with no field cannot end.
**Not yet:** the commit order of a `consume` argument before a later `?` (rule 3),
`?`/`!`/`??`/`?.` on a linear operand (rule 6), `Array.consumeEmpty()` (done in P5a stage 0), the consume commit order for call and initializer operands (done in P5a stage 1; array literals and enum payloads remain), a linear `SendError`/channel element
(no channels yet), and the IR defence in depth.

1. Spec: §4.7, §4.1.1 (linear, `Copyable`), §7.1.2, §2.6 (the four builtin generics, channel
   creation), 0013 amendments.
2. Parser and name resolution: the toolchain-only `linear` modifier.
3. Type checking: `TypeCheckedModel::isLinear` and propagation; the type-level rules above.
4. Semantic analysis: possibly-live, must-consumed projections, exits, temporaries, matches.
5. Lowering defence in depth (later), with the verifier rule and wire bump.
6. Docs and acceptance, with the role reviewers.

### Open questions (P2c, after review)

9. **A public `linear struct`** and its terminal (a `discard`-like builtin or field-by-field).
   Deferred; see the test seam.
10. **`consumeEmpty()` versus a `Result`.** The trap is recommended: the compiler cannot prove
    emptiness, and a `Result` only moves the obligation.
11. **`defer`.** Re-decide after P7.

## Review findings applied

The three role reviews of the first implementation (type system, IR and backend, adversarial)
found: `Dict.get` cloned a noncopyable value (now rejected, `Dict.get` of a noncopyable value
type); a noncopyable local could not be moved into any aggregate (`consume x` now moves it
into a binding, an assignment, an initializer, an enum case, `append` and array or dictionary
elements); a `deinit` that assigned `self` recursed without end (the destroy helper clears the
live flag before running the `deinit`, and `&self = ...` is rejected); copies through `if`,
`match`, blocks, `?` and a trailing expression reached only a lowering error (now type
errors); an `if` or `match` that yields a fresh noncopyable value was refused (its result
moves out of its slot); the noncopyability query was exponential in type depth (memoised per
query); and `-O1` or higher with `-gfull` crashed the compiler on a `deinit` (the destroy
helper that calls it is kept out of line). The IR verifier now also requires that only the
hidden live flag lacks a source symbol and that field lookups never resolve to it.

## Evidence

- The compiler survey above: `prepareOwned` (copy emission), `InitializationBuilder`
  (consumption flow state), `requiresDestruction` and the per-type destroy helpers.
- 0012's open question 1 and its worked example: a pattern binding cannot be consumed,
  so the web server unwraps with `?`.
- The second `language-designer` review of 0013: handles are copyable today, "must be
  consumed" is a linear rule that §4.7 forbids as written, and a noncopyable handle
  without a closing destroy helper leaks its descriptor.

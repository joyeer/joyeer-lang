# 0015: Noncopyable and linear types, and handles that close

- Status: proposed (2026-11-08). Phase P2 of the
  [concurrency plan](../plan/concurrency.md). Design only; nothing is implemented.
  The one fork in it, how a handle closes, was settled the same day in favor of a
  real `deinit` ("The fork"); that revises the answer accepted in
  [0013](0013-concurrency-model.md) question 6, which now says so.
- Spec: none yet. When accepted it amends [§4.1.1](../spec/04-memory.md) (copyability),
  [§4.7](../spec/04-memory.md) (destruction) and [§2.6](../spec/02-types.md) (builtin
  generics), and [§18.6](../spec/18-host.md#186-tcp-networking) (handles).

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
- **Linearity** is a flag on a builtin type and propagates through containers like
  noncopyability. The initialization analysis gains the inverse check: when a block or
  function ends, any linear binding that is initialized and not consumed is
  `semantic.linear-not-consumed`, using the existing merge at branches and loops. `?`
  and early `return` are exits and are checked like any other. A destroy helper for an
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
role reviews and validation. Progress: stages 1 to 8 are implemented (verified on
Linux; a Windows build is still to be run); stage 9 is the review and acceptance run. Stages, with what each must do:

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
   emitted after all source functions are declared. The IR wire version is 11.
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
  returns on an error must join it on that path. There is no `defer` (reserved, §15);
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

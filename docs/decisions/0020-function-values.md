# 0020: Function values

- Status: proposed (2026-11-12), revised after a `language-designer` review whose findings are folded in
  below. Phase 1 of the [generics plan](../plan/0010-generics.md).
- Spec: [§2.3](../../specs/02-types.md#23-composite-types) (function types, made normative). The spec
  change also amends §2.9, §3.1.1, §3.2.1, §4.1.1, §4.4, §5.4, §5.6, §5.8, §5.11, §12.2, §19.4.5,
  §19.4.7, §19.5, §21.2.1 and Appendix C (see "Spec changes").

## Context

[§2.3](../../specs/02-types.md#23-composite-types) has described a function type since v0.1:

```text
function_type   ::= '(' [ param_type , ... ] ')' ':' type
param_type      ::= [ access_effect ] type
```

but no stage implements it. The parser has no function-type syntax, the type model has no function
kind, and a function name is accepted only as a callee or as the `function:` argument of `spawn`, which
is read as a name, not checked as a value (§21.2.1). Everywhere else, using a function as a value fails
late or misleadingly ([concurrency plan](../plan/0009-concurrency.md)).

The runtime already calls through addresses, but untyped: `joyeer_intrinsic_call_void1/2`
([0009](0009-runtime-core-capabilities.md), D2) take an `Int` address, and nothing checks the target's
signature. The backend generates its own typed thunks for tasks (`taskThunk`) and its own clone and
destroy helpers for collections, because Joyeer code cannot name a typed function.

The [generics plan](../plan/0010-generics.md) needs function values first. They are the smallest
step that lets library code accept behavior as a parameter; generic code (Phase 2) builds on them.
[0013](0013-concurrency-model.md) and [0018](0018-tasks.md) already promised that `spawn`'s
`function:` argument is a subset of this feature.

Constraints:

- **No GC, deterministic destruction (§4).** Whatever a function value holds must be owned or freed at
  a known point.
- **Access effects are visible (§0.1, §4.2).** A call through a value must show and check `&` and
  `consume` exactly as a direct call does.
- **Reviewable, AI-authored code (§0).** A reader must be able to tell what a call through a value can
  do from the value's type, and must be able to see that a call is indirect without looking up types.
- **Zero cost.** A call through a value is one indirect call; there is no allocation, boxing or
  reference count.
- Function names are unique per lookup (§12.2: a private declaration may shadow another file's only in
  its own file), so a name always denotes one function; there is no overload resolution to design.

## Options considered

### A. What a function value can hold

1. **Non-capturing references to top-level Joyeer functions only (chosen).** The value is a code
   address and nothing else. It is copyable, needs no destruction, and can cross threads, because it
   refers to no state.
2. **Closures that capture by move.** The value would need a context block. That raises ownership
   questions with no settled answer yet: who frees the context, whether a closure that captured a
   noncopyable or linear value is itself noncopyable or linear, how `inout` captures are prevented, and
   how a context crosses threads. §5.11 defers closures to v0.2, and nothing in Phases 1 to 4 needs
   them: generic code passes behavior through protocol-bounded type parameters instead. The evidence
   that would reopen this: library APIs (sorting, searching, visitors) that keep needing a struct plus a
   free function to carry one value of context, measured in real code.
3. **Methods as values (`Type.method` or `value.method`).** An unbound method is a function whose
   first parameter is the receiver, but its receiver effect (`borrowing`, `mutating`, `consuming`) has
   no spelling in `param_type` today. A bound method captures its receiver, which is option 2. Deferred.

### B. Labels in the type

1. **No labels; calls through a value are positional (chosen).** `(borrowing String, inout Int): Bool`
   is the whole type. This follows §2.3's grammar, keeps one function type per shape, and lets any
   function with matching effects and types be used, whatever its parameter names are. Without closures,
   labels in the type would force an adapter function for every label mismatch.
2. **Labels are part of the type** (`(name: String, count: inout Int): Bool`). Calls would keep labels,
   but two functions differing only in parameter names would have different types and could not be
   stored in the same variable, and the type gets noisier.
3. **Optional labels as documentation only**, ignored by the type. Rejected: a label that the compiler
   neither checks nor requires misleads the reader.

The cost of option 1 is real: two parameters of the same type (`(Int, Int): Int`) can be swapped
silently in a call through a value. Mitigations: the positional form is limited to calls through a
value (a direct call by name keeps its labels, and `f(x)` on a function named `f` keeps today's
missing-label diagnostic); access markers stay required; and option 1 of C below makes every indirect
call through a field or element visibly different from a method call.

### C. Calling a value stored in a field or element

`s.f(x)` is a method call today, and `&s.f(...)` / `consume s.f(...)` are receiver markers (§4.3,
§21.3). A field of function type would make `s.f(x)` mean either.

1. **A callee that is not a plain identifier is parenthesized (chosen):** `(s.handler)(x)`,
   `(table[i])(x)`, `(maybe!)(x)`. `s.f(args)` always names a method. A stored field and a method of the
   same type with the same name is an error. `&` or `consume` before a parenthesized-callee call is an
   error, because the markers belong to the arguments. Rust uses the same form.
2. **Look up fields first, then methods.** Rejected: the reader could not tell an indirect call from a
   method call without the type, and the receiver markers would change meaning.

### D. Representation

1. **A code address with the Joyeer calling convention (chosen).** Same size as a pointer. A call is
   one indirect call to the function the compiler already emits, and no wrapper is generated. This
   requires the Joyeer convention to be determined by the function type alone (see "IR and backend").
2. **A pointer to a descriptor (address plus context slot)**, ready for closures. That would double the
   size and add a load to every call for a feature that is not designed yet. If closures arrive, they
   can be a separate type or change this representation behind the same source syntax, since nothing
   in the language exposes the bits.

## Decision

### Function types

`(E1 T1, ..., En Tn): R` is a type. Each `Ei` is an access effect: `borrowing` (the default, and the same
type when written out), `inout`, `consuming` or `initializing`. Each `Ti` is any type except `Never`. `R`
is any type, including `()` and `Never`. The result is mandatory: write `(): ()` for a function that takes and returns nothing.
Parameters have no labels.

Two function types are the same type when the parameter count, every effect, every parameter type and
the result are identical. There is no subtyping, no conversion and no variance between function types.
This is deliberate even for `Never` (§2.9 makes `Never` a subtype of every type for expressions, not for
function results): a `(Int): Never` value is not a `(Int): Int` value.

Grammar (§2.3 and Appendix C):

```text
type            ::= primary_type [ '?' ]
primary_type    ::= primitive_type | '(' ')' | nominal_type [ generic_args ]
                 |  array_type | dict_type | function_type
                 |  '(' function_type ')'
function_type   ::= '(' [ param_type , ... ] ')' ':' type
param_type      ::= [ access_effect ] type
```

- The result type extends as far right as possible: `(Int): Int?` returns `Int?`, and
  `(Int): (Int): Int` returns a function. An optional function is written `((Int): Int)?`.
- Parentheses around a type are allowed only around a function type.
- Inside `[` `]`, a `(`…`)` followed by `:` is a function type, so `[(): Int]` and `[(Int): Bool]` are
  arrays of functions; a dictionary with such a key would need parentheses and is rejected anyway,
  because function types have no equality.
- A labeled parameter (`(x: Int): Int`) and a missing result (`(Int)`) are errors with their own
  diagnostics.

### Function values

The name of a function, used as an expression where it is not called, is a value of its function type.
`let f = square` infers `f`'s type. A module-qualified name works too, wherever it is visible (§12).
Being a value does not widen visibility: only the name is checked, so a private function's value may
leave its file through a public function that returns it, as in Rust and Swift.

These are not values, each with its own reason in the diagnostic:

- methods (a hint suggests a free function), declared and synthesized initializers, and enum cases;
- the compiler-known builtins (`print`, `spawn`, `channel`, `byteToInt`, `byteToString`) and the
  runtime intrinsics (§19.4.5). A user declaration that shadows a builtin is an ordinary function and
  can be a value;
- `@c_extern` and `@c_data` declarations: their convention is C, and a `@c_data` "function" yields an
  address and calls nothing (§19.4.7). A typed C callback is a separate design (§19.10);
- the program entry point, the root module's `main` (§3.2.6), as for a `spawn` target (§21.2.1). This is
  a language choice, not a backend constraint: the backend lowers `main` as an ordinary function. A
  dependency's `main` is an ordinary function and can be a value. (The spec does not say whether the
  root `main` may be called directly, and the compiler accepts it; see "Open questions".)
- generic functions, when they exist (§20.4).

A `@c_export` function can be a value; the value calls the Joyeer function, not the export adapter.
(A `spawn` target still cannot be `@c_export`, §21.2.1; `spawn` stays a subset.)

A function's default parameter values (§3.2.5) do not carry over: a value of its type requires every
argument.

### Calls through a value

- A value named by a plain identifier is called directly: `f(a, &b, consume c)`. Any other callee is
  parenthesized: `(s.handler)(x)`, `(table[i])(x)`, `(maybe!)(x)`.
- The arguments are positional. A label in a call through a value is an error, and so is mixing labeled
  and unlabeled arguments in any call. Appendix C's `call_arg` becomes
  `[ label ':' ] [ '&' | 'consume' ] expression`.
- Each argument is checked against its parameter's type and effect exactly as for a direct call (§4.2):
  `inout` and `initializing` arguments are written `&x`, `consuming` arguments `consume x` (or a literal
  or temporary), with the exclusivity and initialization rules of a direct call.
- **Evaluation order.** The callee expression is evaluated first, to a copy (which is trivial); no
  access to its storage is held while the arguments are evaluated or during the call. The arguments are
  then evaluated left to right and committed as for a direct call (§4.4, §4.7.1). So `(s.f)(&s)`,
  `(handlers[0])(&handlers)` and `(s.f)(consume s)` are valid.
- A call through a value whose result is `Never` does not return, as for a direct call.
- An optional function value must be unwrapped first: `(maybe!)(x)` or a `match`. `maybe?(x)` already
  means "propagate `None` out of the enclosing function, then call" (§5.4.1); the diagnostic for calling
  an unwrapped optional says so, because a Swift reader expects "call if present".

### Ownership and other rules

- A function value is copyable, has no `deinit`, and is never linear. It is trivially destroyed.
- Copyability and linearity do not look inside function types (§4.1.1, decision 0015):
  `(consuming TcpConnection): ()` and `(consuming JoinHandle<Int>): Int` are copyable and not linear. A
  struct holding a function value stays copyable unless it declares `deinit`, and
  `struct S { var f: (S): S }` is not a recursive layout.
- Function types have no equality: `==` is rejected, and so is a dictionary key of function type (the
  general key rule already admits only `Int`, `Bool`, `UInt8` and `String`). Function identity is not
  stable across optimization (merged functions), so equality would be unreliable.
- `print(value:)` rejects a function value; it accepts only primitives and strings.
- A function value may be stored in a binding, a struct field, an enum payload, an `Optional`, an array
  or a dictionary value. It may be a task's input or result and may be sent through a channel, because
  it carries no state.
- A top-level constant (§3.1.1) of function type is not supported in this phase and has a diagnostic.
  A function address is a link-time constant, so this is a cheap later extension.
- Function types are exposed types (§12.2): a public declaration whose type mentions a function type
  must have every parameter and result type at least as visible.
- Core units (§19.4.7) may use function types and values; nothing in them needs the managed runtime.

### `spawn`

Unchanged. `function:` still takes a function's name (§21.2.1). Accepting any function value there
needs `spawn` to be generic over its input and result, which is Phase 2 or later, and every program
accepted now stays valid.

### IR and backend

A function type's record in the IR type table holds its parameter types and result as type arguments
(`[params..., result]`) and one effect per parameter. Two IR instructions:

- **Function address.** Names a callee and has no operands. The callee must be a Joyeer function that can
  be a value (not external, not `@c_data`, not a prelude function accepting any type, not the root
  `main`), and the result is a value whose function type equals the callee's signature exactly, effects
  included.
- **Call through a value.** Operand 0 is a value of function type, followed by one operand per
  parameter. Each operand's category is `address` for `inout` and `initializing` and `value` otherwise,
  with the parameter's type. A result is present exactly when the result type is neither unit nor
  `Never`, and a call whose result is `Never` is followed by `unreachable`.

The verifier checks all of the above, plus `effects.size() + 1 == arguments.size()` and every effect
value in range for function types (the wire reader does not range-check enums). It also requires the
`unreachable` after a `Never` call for direct calls, which today only the lowering guarantees. As for
direct calls, the verifier sees effects only as categories; whether a `consuming` argument is really
moved is an ownership property of the lowering, which must key off the function type's effects rather
than a callee's parameter list.

The backend lowers a function type to an opaque pointer (layout 8 bytes, aligned 8, without looking into
its parameter types), a function address to the function's `joyeer_fn_<id>` symbol, and a call through a
value to one indirect call.

**Invariant.** A Joyeer function's LLVM function type, calling convention and attributes come from one
helper over (effects, parameter types, result type), used both by function definitions and by indirect
calls; any future per-function attribute must be derived from the type and repeated at indirect call
sites. This holds today ([native_types.cpp](../../compiler/backend/native_types.cpp) `declareFunction`:
`ccc`, no attributes; `inout` and `initializing` passed as `ptr`, `borrowing` and `consuming` by value; a
unit or `Never` result as `void`), but only by convention. A mismatch would be undefined behavior that
the -O2 pipeline turns into `unreachable` after devirtualization, so a unit test asserts that Joyeer
function definitions carry no calling convention or attribute beyond what the type implies. Taking a
function's address also stops the optimizer from changing its convention, which it may otherwise do for
internal functions.

Further rules from the backend review:

- `Never` cannot be a parameter type of a function type: its LLVM type is `void`, which is not a valid
  parameter, and such a function could never be called.
- A call through a value is never implicit code and always has a debug location and scope when its
  function has one. At -O2 a call whose target is known becomes a direct call and may be inlined; without
  a location, the inlined code would keep the callee's debug scopes.
- Debug info describes a function type as a pointer to a subroutine type built like the function
  signatures. `Never` must not appear after position 0 in that array, where a null entry means variadic
  (excluded by the previous rule).
- `@c_export` functions already have a separate Joyeer-convention function beside the export adapter.
  The verifier rejects any native binding outside runtime context, so this matters only for runtime and
  core units.
- The root `main` is an ordinary Joyeer-convention function in every entry mode (C startup and
  `joyeer_start` both reach it through generated adapters), so its exclusion is a language decision
  only, not a backend constraint.
- Functions of every program module share one LLVM module, so a dependency's function can have its
  address taken. Runtime-object functions are internal and not visible to user code.
- The IR wire format version goes from 15 to 16, with a round-trip test of a function type and both
  instructions. `JOYEER_BACKEND_ABI_VERSION` does not change, because the C structs do not, and the
  `joyeer-backend` C ABI still exposes no LLVM types.
- No control-flow-integrity scheme (`/GUARD:CF`, CET/IBT, BTI) is enabled today. If one is enabled later,
  every object, the runtime included, must set the module flag, and LLVM then marks address-taken
  functions as valid targets.

Phase 4 of the generics plan passes thread and channel callbacks to a runtime written in Joyeer, so it
can use function values directly. It may assume only Joyeer-to-Joyeer compatibility within one toolchain
build: the Joyeer convention passes first-class aggregates and an unextended `i1`, which matches a C
function pointer only by accident. A runtime-only conversion from a function value to an `Int` address is
not needed now; if Phase 4 finds that it is, it gets its own record.

### Spec changes

- §2.3 and Appendix C: the grammar above, a `type` production, and `call_arg`.
- §2.9: no `Never` variance for function types, as above.
- §3.1.1: top-level constants of function type are not supported.
- §3.2.1, §5.6: "no positional call form" becomes "no positional form for a call by name"; calls through
  a value are positional.
- §5.8: enum payloads are no longer "the only unlabeled invocation form".
- §4.1.1: copyability and linearity stop at function types.
- §4.4: evaluation order of a call through a value.
- §5.4: `s.f(args)` always names a method; parenthesized callees; no field and method with one name.
- §5.11: function values exist; closures stay deferred.
- §12.2: function types in the exposed-type list.
- §19.4.5: write the intrinsic table's signatures as declarations, not as function-type look-alikes.
- §19.4.7: core units may use function types.
- §19.5: function values are Joyeer-only; C callbacks are deferred.
- §21.2.1: the entry-point exclusion names the root module's `main`.

## Open questions

1. **Direct calls to the root `main`.** The compiler accepts them and the spec is silent. Either state in
   §3.2.6 that they are allowed, and then let `main` be a value too, or forbid both. The backend has no
   objection either way: `main` is an ordinary Joyeer-convention function in every entry mode.

## Consequences

- Easier: library code can accept behavior as a parameter (a comparator, a visitor, a local handler
  table), and later phases can build on typed calls instead of untyped intrinsic addresses.
- Harder: a call through a value cannot be inlined unless the optimizer proves the target, and a reader
  must follow the value to know which function runs. The type states the full effect signature, and a
  parenthesized callee or a binding of function type marks the call as indirect.
- New diagnostics: a function that cannot be a value, with its reason (`not-a-function-value`); a
  labeled call through a value (`labeled-function-value-call`); mixed labeled and unlabeled arguments; a
  labeled parameter in a function type; a function type with no result; calling a value that is not a
  function; the wrong number of arguments through a value; calling an optional function value without
  unwrapping; a non-identifier callee without parentheses that names no method; a field and method with
  the same name; `==`, a dictionary key, or `print` of a function value; a top-level constant of
  function type. Stage prefixes follow the existing scheme (`parser.` or `type-checking.`).

### Tests

- **Parser:** `(): ()`; `(()): Int`; `(Int): Int?` versus `((Int): Int)?`; `(Int): (Int): Int` nests to the
  right; `[(): Int]` and `[(Int): Int]` are arrays; `[String: (Int): Bool]`; `Optional<(Int): Int>` and
  `Result<(Int): Int, E>`; each effect keyword in parameter position; rejected: labeled parameters, a
  missing result, mixed labeled and unlabeled arguments, a parenthesized non-function type.
- **Type checking:** exact matching, with one mismatch each in effect, count, parameter type and result;
  `borrowing` written out equals the default; no `Never` variance; `let f = square` infers; a defaulted
  function's value needs every argument; each rejected value kind with its reason (method, declared and
  synthesized init, enum case, builtin, `@c_extern`, `@c_data`, intrinsic, root `main`); accepted: a
  dependency's `main`, a user function shadowing a builtin, a `@c_export` function, a qualified name; a
  labeled call through a value; missing `&` or `consume`; `==`, a dictionary key and `print` rejected; a
  top-level constant rejected; an optional call without unwrapping rejected and `f?(x)` propagating;
  a private function's value returned by a public function accepted; a public function type exposing a
  private type rejected; a field and method with one name rejected.
- **Ownership and exclusivity:** `(s.f)(&s)`, `(a[0])(&a)`, `(s.f)(consume s)`; use after `consume`
  through a value; `initializing` through a value with definite initialization afterwards; a struct
  holding a function value is copyable, and with `deinit` noncopyable; `(consuming JoinHandle<Int>): Int`
  is copyable and not linear; `struct S { var f: (S): S }` is accepted.
- **IR verifier:** each rule of "IR and backend" rejected by its own invalid-IR test: a mismatched
  callee type, a non-addressable callee, a wrong argument count, a wrong operand category, a result
  present or missing, a `Never` call without `unreachable`, effect count and range in the type table.
- **Backend:** a wire-format round trip of a function type and both instructions; Joyeer function
  definitions carry no calling convention or attribute beyond what the type implies.
- **End to end, both optimization profiles:** function values in structs, enums, `Optional`, arrays and
  dictionary values; sent as a task input and through a channel, then called on the other thread; a
  call through a `Never` value; recursion through a value; a debug-info stepping and type-printing test.
- **-O0/-O2 differential:** one program storing functions of every effect (`inout` Int, `consuming`
  String, `initializing` String, borrowed struct, unit result, `Never` behind a branch) in a field, an
  array, an `Optional` and an enum payload, calling each, including `(s.f)(&s)`; identical stdout and leak
  check at -O0 and -O2, and a `-g -O2` build that passes the LLVM verifier after the pipeline.
- **Compatibility:** every existing §21 `spawn` fixture passes unchanged.

## Evidence

- Swift and Rust: function types without labels, positional calls through a value, and labels only on
  declarations. Rust's `fn` item-to-pointer coercion is the non-capturing case chosen here, and Rust's
  `(s.f)(x)` is the parenthesized-callee rule.
- The concurrency work found that a function used as a value gives a late or misleading diagnostic
  ([0009 plan](../plan/0009-concurrency.md)); this record replaces that with a defined feature.
- The backend already generates per-type thunks and helpers that Joyeer code cannot express
  ([native_collections.cpp](../../compiler/backend/native_collections.cpp)). Typed function values are
  the first step to moving them into the library (plan 0010, Phase 4).
- Review: a `language-designer` review (2026-11-12) found six blocking gaps (field-call syntax, type
  grammar, the list of amended sections, the `initializing` exclusion, the exact rejected kinds,
  evaluation order) and ten risks. All are resolved above; its five alternative choices were adopted:
  `initializing` included, parenthesized callees, only the root `main` excluded, parentheses only around
  function types, and the callee evaluated first.
- Review: an `ir-backend-reviewer` review (2026-11-12) confirmed the one-indirect-call representation
  against the backend, corrected two statements (`Never` results get no `noreturn`; the convention
  invariant is held only by convention), and added the verifier rules, the `Never`-parameter and
  debug-location rules, the wire-format bump and the backend tests above.

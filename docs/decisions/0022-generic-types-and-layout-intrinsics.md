# 0022: Generic structs and enums, and runtime layout intrinsics

- Status: accepted (2026-11-13), after a language-designer review (see "Review"). Phase 3 of the [generics plan](../plan/0010-generics.md); it settles that
  plan's open question 5. Builds on [0021](0021-protocols-and-generic-functions.md) (generic functions,
  instantiation) and [0005](0005-runtime-unsafe-memory.md) (runtime-only unsafe layer).
- Spec: §20.12 (new), with amendments to §2.6, §3.2.3, §3.3, §3.4, §3.5, §4.1.1, §19.4.5, §20.10 (item 1 lifted).

## Context

Phase 4 moves `JoinHandle<R>`, `ChannelSender<T>` and `ChannelReceiver<T>` out of the compiler into Joyeer
library types, and a later plan does the same for `Optional`, `Result`, `Array` and `Dict`. That needs two
things 0021 deliberately left out:

1. User `struct` and `enum` declarations with type parameters, and methods on them.
2. Code that owns raw memory holding `T` values: a queue or array implemented in the runtime source must
   know `T`'s size and alignment, move a `T` into and out of an address, and destroy a `T` at an address.
   Today runtime code sees only `Int` addresses and the compiler supplies this knowledge through opcodes.

What the compiler does today (read, not assumed):

- A struct or enum type is `TypeKind::structure`/`enumeration` interned by symbol, with an empty argument
  list; `TypeRecord::arguments` and `substitute` already exist for builtins and function types.
- 0021 types a generic function body once against `TypeKind::typeParameter`, discovers
  `(function, type arguments)` instances after type checking, and lowers each instance with a
  substitution. IR, verifier and backend see only concrete types.
- `isCopyable`, `isLinear` and `noncopyableReason` walk struct fields and enum payloads with a visiting
  set; they are the place a generic type's rules go.
- `generic-native-binding` forbids generics on `@c_extern`, and `joyeer_intrinsic_*` symbols are
  `@c_extern` imports with an exact scalar signature (0005).
- There are no explicit type arguments at call sites (§20.10 item 6): `f<T>(x)` is ambiguous with
  comparison.

## Options considered

### A. How a generic type is checked and lowered

1. **Same model as generic functions (checked once, instantiated by substitution).** A declaration's
   fields, cases and methods are typed against its parameters; each distinct `Box<Int>` is a concrete IR
   struct; each method is an instance keyed by `(method, type arguments of the type)`.
2. **Template semantics.** Rejected for the reasons in 0021 A1.

### B. Writing the type at a construction site

1. **Infer from the initializer arguments only.** `Box(value: 1)` works; `Stack()` and `Optional`-like
   `Empty` cases cannot be written without a value.
2. **Infer from arguments, then from the expected type** (a binding annotation, a parameter, a return
   type, a field, an assignment target). `var s: Stack<Int> = Stack()` and `return Tree.Leaf` work.
3. **Explicit arguments in expressions** (`Stack<Int>()`). Needs the §20.10 item 6 disambiguation; a
   larger lexical decision than this phase needs.

### C. Linear values in generic types (plan question 5)

1. **No linear type argument anywhere in user generics** (0021's rule extended to types). A
   `linear struct JoinHandle<R>` is allowed, because linearity is declared on the type, not supplied as
   an argument; `R` is affine.
2. **Allow linear arguments with a way for generic code to end a value** (a requirement such as
   `Consumable`, or an intrinsic). Needed only when a library `Array<T>` must hold a `JoinHandle<R>`;
   builtin `Array` stays in the compiler through Phase 4, so nothing in this plan needs it.
3. **Allow linear arguments and check the body linearly.** Rejected in 0021 C3: generic code cannot end
   a `T`.

### D. The layout intrinsics' surface

1. **Generic `@c_extern` symbols** (`joyeer_intrinsic_size_of`). Reuses authorization and wire transport
   but reverses `generic-native-binding` for one reserved family, and an intrinsic with no `T` in its
   arguments (`size_of`) cannot infer `T`.
2. **Opcodes plus compiler-known generic functions, no new syntax**, with `T` always named by a witness
   argument (`size_of(of: sample)`). Needs a `T` value where none exists (a buffer that is empty).
3. **Compiler-known generic functions, implicitly in scope in authorized units, with explicit type
   arguments** at their call sites only (`joyeer_intrinsic_size_of<T>()`). Not `@c_extern`; the parser
   applies the type-argument rule only in authorized units, so no ordinary identifier changes meaning and
   §20.10 item 6 stays deferred.

## Decision

Options A1, B2, C1 and D3, as the following rules.

### Generic structs and enums

- `struct Name<T, U: P & Q>`, `linear struct`, and `enum Name<T>` take type parameters with the §20.4
  constraint syntax. A parameter is a distinct type inside the declaration, noncopyable unless its
  constraint lists `Copyable`, never linear, and may not be `Never` (0021). `where` clauses stay out.
- **Use.** A generic type is written with type arguments in every type position (`Box<Int>`, `Tree<[T]>`).
  Omitting them, giving the wrong number, or giving a protocol or a parameter that was given arguments is
  `type-checking.invalid-type-arguments` (extends `invalid-generic-declaration`'s message set).
  Each argument must satisfy its constraints (`unsatisfied-constraint`).
- **Construction infers type arguments** by unifying initializer (or enum-case payload) arguments with
  the field/payload types (the §20.4.1 rules), and uses the expected type (B2) as follows. This is a
  deliberate, narrow exception to §20.4.1's "no return-type-only inference", which §20.4.1 is amended to
  state: it applies only to **initializer and enum-case construction of a generic type**, because there
  is no explicit `Stack<Int>()` form; a generic *function* call still never infers from its result.
  - *Contexts that provide an expected type:* a binding annotation, a `return` in a function with a
    declared result, an assignment target or field initializer of known type, and an argument whose
    parameter type is known without inference, that is, a parameter of a non-generic callee or one whose
    type does not mention a parameter still being inferred. A parameter of a generic callee does not
    provide one (circular).
  - *Order:* non-literal arguments are unified first; the expected type then binds still-open
    parameters; only then do literals take their default types. So `var b: Box<Double> = Box(value: 1)`
    gives `Box<Double>`. An expected type never overrides a type that non-literal arguments determined;
    disagreement is `conflicting-type-argument`.
  - *Optional wrapping:* an expected `Box<Int>?` or `Tree<Int>?` supplies `Box<Int>`/`Tree<Int>` to the
    construction (one `?` level, as §2.4 wrapping does).
  - *No-information arguments* (`nil`, `[]`, `[:]`, `.Case`) contribute nothing; the expected type may
    fill them (`let b: Box<Int?> = Box(value: nil)`).
  - If a parameter is still open: `cannot-infer-type-parameter`, naming it and suggesting an annotation.
- **Methods, `init`, `deinit`, subscripts and conformance extensions** of a generic type are written in
  the declaration or in `extension Name<T>`, where `T` is bound by that header (the names are free,
  not required to match the declaration's) and has the declaration's constraints. They are checked once
  against the parameters. A method may not declare its own type parameters
  (`invalid-generic-declaration`); a generic method is §20.10 item 1's remaining half and stays deferred
  with `where`.
- **Extension headers.** `extension Name<A, B>` lists exactly the declaration's parameters, once each, as
  fresh names bound to them in order; the names need not match the declaration's. A concrete argument
  (`extension Box<Int>: P`, which would be specialization, §20.6), a repeated name, a wrong count, or
  restated or loosened constraints (`extension Box<T: Q>`) is `type-checking.invalid-extension-header`;
  the parameters carry the declaration's constraints, nothing else. This holds for methods in the
  extension as well as conformances.
- **Conformance is unconditional.** `extension Box<T>: P` states that `Box<X>` conforms for every `X` that
  satisfies `Box`'s own constraints. Conditional conformance (`where T: P`) stays deferred. The
  conformance key is `(type symbol, protocol)`, so two `extension Box<T>: P` / `extension Box<U>: P` in
  a module are `duplicate-conformance`. Witness bodies are checked once against the parameters'
  declared constraints, which is what makes the conformance sound for every `X`. Entailment (0021,
  §20.4.2) is amended: inside a body, `Box<T>` satisfies `P` when such an extension exists (and `T`
  satisfies `Box`'s constraints), whereas a *builtin* container built from a parameter (`[T]`, `T?`)
  still satisfies no user protocol.
- **`Self`, static members.** In a generic type's body and extensions `Self` means `Name<A, B>` with the
  header's parameters (§3.9 is amended; it is implemented only for non-generic types today). `Name<X>.m`
  and `Name<X>.Case` are not expression forms (same ambiguity as explicit arguments); a case or static
  member is written through the expected type (`.Leaf`) or `Name.Case(...)` with inference. Static
  stored members of a generic type are not accepted in v1 (`invalid-generic-declaration`).
- **Memberwise init.** A generic struct's synthesized initializer has the field types in terms of the
  parameters, participates in the inference above, and follows §3.3's visibility (a `private` field makes
  it private) and §3.7 (a `deinit` type's rules) unchanged.
- **Builtin-named user types.** A user `struct Array<T>` (or `Optional`, `Result`, `Dict`) is an ordinary
  generic type in its scope (§21.9.1: user declarations win) and is checked with its own arity.
- **Copyable and destruction** (§4.1.1, which gains a generic-type paragraph): `Box<X>` is copyable
  exactly when the declaration has no `deinit` and every field or payload, with parameters replaced by
  `X`, is copyable. A declared `deinit` makes every instance noncopyable. A `deinit` runs once per
  instance value (§4.7), then its fields are destroyed. In the declaration, a parameter's own copyability
  is its constraint's (`T: Copyable`), as in 0021. A parameter that appears in no field (phantom) does
  not affect copyability. **A type that owns `T` values through an `Int` address has no `T` field, so
  field-based inference would make it copyable and a copy would double-destroy: such a type must
  declare `deinit`.** The compiler cannot detect this; it is a runtime-source rule, stated in the
  runtime notes and checked by the `Queue<T>` fixture (which also proves a copy of it is rejected).
- **Recursive generic enums** (`indirect case`) work as non-generic ones do. Expanding recursion
  (`type-checking.expanding-generic-recursion`, 0021) is extended: its call graph gains an edge for
  every construction of a generic type, every method or initializer call on one, every conformance
  witness, and every field, signature and function-typed field that names a generic type, labeled with the
  substitution. A cycle where a parameter maps to a type strictly containing a parameter of the cycle
  (`struct S<T> { var next: S<[T]>? }`; a method of `Box<T>` that builds `Box<[T]>` and calls itself;
  two types doing so through each other) is rejected once, statically, whether or not a root reaches it.
- **Instantiation.** The instance set gains `(type symbol, type arguments)` for layouts, found from
  every concrete type reached by a root; methods are instances keyed by `(method, arguments of its
  type)`. The same caps, cache, report, naming (`Box<Int>`, `Box<Int>.get`) and debug scopes as 0021
  apply, and `--report-instantiations` lists type instances beside function instances. IR struct and
  enum types are concrete, named by the display name; the verifier rejects a type parameter in one.
- **Public API:** an exposed signature of a generic type may mention only protocols and types at least as
  accessible (§12.2), as 0021.

### Linear values (question 5)

C1. A generic *type* cannot take a linear type argument (`type-checking.linear-type-argument`), the same
rule as for functions, and `Any` and `Never` are not type arguments either. A `linear struct` may itself
be generic: its parameters are affine (amended by [0023](0023-library-concurrency.md): the closed toolchain-prelude `JoinHandle<R>` accepts a linear `R`), and it must be ended as any linear struct (0015); as today,
`linear` is honored only in toolchain-authorized sources (§4.1.1). Consequently `JoinHandle<R>` can be a
linear generic struct, and a user `Stack<T>` can never hold one (so a user `Wrapper<T>` cannot wrap
`[JoinHandle<Int>]`: a stated limitation). Builtin `Array`, `Dict`, `Optional` and `Result` keep their
compiler rules for linear elements until they move to the library, which needs C2 and is a separate
decision (the Phase 4 follow-up plan). Where both apply, the specific existing ID wins
(`linear-collection-element`, `linear-in-deinit-type`, `linear-channel-element`, §21), since
`ChannelSender<T>` keeps §21's diagnostics; `linear-type-argument` is reported for user generics.
Whether a library generic struct that owns an address is sendable into `spawn`/channels is decided in
Phase 4 against §21's input rules; Phase 3 adds no sendability rule.

### Layout intrinsics

Five **compiler-known generic functions**, not `@c_extern` imports (so §19.4.5's exact-scalar-signature
rule and `generic-native-binding` are untouched; both sections are amended to say these five are a
separate family). They need no declaration: they are implicitly in scope, by their reserved names, in
toolchain-authorized units only (the gate of 0005). Elsewhere a use is
`type-checking.unauthorized-intrinsic` at the use site. They are not function values (§5.6.1) and may
not be named without being called. They never appear in a public signature (§12.2), so Phase 4 does not
leak them. No pointer type is added; an address is an `Int`. `T` is any type that is not `Never`, `Any`
or linear.

| Function | Meaning |
|---|---|
| `joyeer_intrinsic_size_of<T>(): Int` | Size in bytes of one `T` in memory: a multiple of its alignment, identical for every use of the same concrete type in a program. |
| `joyeer_intrinsic_align_of<T>(): Int` | Alignment in bytes. |
| `joyeer_intrinsic_move_in<T>(address: Int, value: consuming T)` | Stores `value` at `address`; the slot now owns it. |
| `joyeer_intrinsic_move_out<T>(address: Int): T` | Returns the value at `address` as an owned `T`; the slot is uninitialized afterwards. |
| `joyeer_intrinsic_destroy_at<T>(address: Int)` | Destroys the value at `address` as a dropped `T` would be (a `deinit`, then fields, releasing heap storage); the slot is uninitialized afterwards. |

- **Surface.** Explicit type arguments are accepted only on these names, in authorized units only: the
  parser applies the rule only when the source is toolchain-authorized, so in an ordinary program
  `joyeer_intrinsic_x < y > (z)` is an ordinary comparison of ordinary identifiers, and the prefix is not
  newly reserved there. Appendix C gains `intrinsic_call ::= INTRINSIC_ID '<' type , ... '>' '('
  arguments ')'`. A `>>` token inside the type arguments is split in type context (as `Box<Box<Int>>`
  already must be; both are parser tests).
- **Semantics.** `Void` has size 0 and alignment 1; `Bool` and every other type has its store size.
  `Void` makes `move_in`, `move_out` and `destroy_at` no-ops; a struct without stored fields is accessed like any other value. Every Joyeer value is bitwise
  relocatable (nothing stores its own address), which is what lets library code grow a buffer with
  `memcpy`/`memmove`. The address must be aligned for `T` and, for `move_in`, hold no live value;
  nothing checks either. `destroy_at` is equivalent to discarding the result of `move_out` and is kept
  for readability. Pairing slots with values is the runtime author's duty, as with a malloc'd buffer in
  0005: a leaked slot leaks, a second `destroy_at` is undefined. No safety claim.
- **Ownership checks.** The type checker types each call at the concrete `T` named or inferred from
  `value`. `move_in`'s `value` is an ordinary `consuming` argument: it goes through exclusivity,
  use-after-consume and `prepareOwned` (a borrowed argument is copied, so `T` must be copyable there) like
  any call. `move_out` yields an owned temporary. A constant array moved in through a library `send`
  keeps §21.2.2's rule because the `consuming` argument is cloned at the boundary; a fixture covers it.
- **IR and backend.** IR gains four operations carrying a concrete type operand (size, align, move,
  destroy; move_in is a typed store and move_out a typed load); the verifier checks the type is concrete,
  not `Never`/`Any` and not linear. The backend emits the target store size and alignment as constants, a
  typed store/load, and the existing per-type destroy helper. A layout the backend cannot size is an
  internal error naming the instance.
- **Known gap.** There is no borrowing access to a slot (`borrow_at`), so a library `Array<T>`
  subscript, `first` or `peek` cannot be written without moving. Phase 4's channels do not need it; the
  `Array`/`Dict` follow-up plan does, and records the addition. This phase does not claim to delete
  `Array` opcodes.

## Spec changes

§2.6, §3.2.3, §3.3, §3.4, §3.5 (generic types, extension headers), §3.9 (`Self` in generic types), §4.1.1
(generic-type copyability and the owner-needs-`deinit` rule), §5.6.1 (intrinsics are not function
values), §19.4.5 (the five generic intrinsics are a separate family), §20.4 and §20.4.1 (types may be
generic; expected-type inference for construction only), §20.4.2 (entailment through an unconditional
extension), §20.8 (new IDs), §20.10 (item 1 partly lifted: generic methods and conditional conformance
remain) and Appendix C (`generic_params` on `struct_decl`/`enum_decl`/`extension_decl`, `intrinsic_call`).

## Consequences

- Easier: a `Queue<T>` or channel in Joyeer owns a buffer of `T` with correct destruction of queued
  noncopyable values, which is what Phase 4 needs to delete the task and channel opcodes.
- Easier: no new runtime concept; the unsafe surface is one visible, prefixed family of five names.
- Harder: type checking, name resolution and lowering carry a substitution for types as well as for
  functions; layouts and debug types per instance; `isCopyable`/`isLinear` need the instance's
  arguments.
- Harder: no `Stack<Int>()` in expressions; an empty construction needs an annotation or other expected
  type.
- Not possible in v1: linear type arguments, generic methods, conditional conformance, explicit type
  arguments in user code, `size_of` of a type with layout the backend cannot compute (reported as an
  internal error naming the instance).

### Tests

- Parser: generic struct and enum declarations, `extension Name<T>`, constraints, `joyeer_intrinsic_*`
  call-site type arguments and the comparison `a < b > (c)` staying unchanged.
- Name resolution: parameters scoped to the declaration and its extensions; `T` used in a static context.
- Type checking: arity and constraint errors; each inference rule and failure; expected-type inference and
  its conflict; linear, `Never`, and `Dict`-key arguments; copyability of `Box<T>` with and without
  `deinit`/`Copyable`; expanding recursion through a field; method-level parameters rejected;
  unconditional conformance with and without a witness; layout intrinsics outside an authorized unit.
- Instantiation: layout cache hits, nested instances (`Box<Box<Int>>`), methods of a type reached only
  through a protocol witness, the caps, order independence, report lines.
- IR/backend: concrete named struct types, verifier rejection of a parameter in a type or of a bad
  intrinsic operand, wire round trip, size/align constants for scalar, struct, enum and array types,
  debug names.
- End to end (`tests/e2e/`): generic struct and enum round trips at -O0 and -O2; the plan's
  fixture, a runtime-source `Queue<T>` over a raw buffer that destroys queued noncopyable values
  exactly once (counting `deinit`s) and leaves no allocation (existing allocation-balance check).

### Diagnostics

Those of 0021 plus `type-checking.invalid-type-arguments` (arity, omitted or misapplied arguments; distinct from 0021's
singular `invalid-type-argument`, which stays for `Never` and a `Dict` key), `invalid-extension-header`,
`unauthorized-intrinsic`, and an extended `expanding-generic-recursion`. `invalid-generic-declaration`
covers method-level parameters (its message names §20.10 item 1) and static stored members. All rows are
added to §20.8.

## Open questions

1. Whether `Optional` and `Result` can be generic enums without special syntax (`T?`, `?` propagation,
   `match` exhaustiveness): the Phase 4 follow-up plan.
2. How generic code ends a linear value (C2), needed before a library `Array<T>` can hold
   `JoinHandle<R>`.
3. Whether size/align should also be available to user code as a safe query (no use found yet).

## Review

`language-designer` (2026-11-13) found five blockers and seven should-fix items, all folded in above:
expected-type inference now has a pinned rule (contexts, order before literal defaults, optional
wrapping, no-information arguments) and is a stated narrow exception to §20.4.1; the intrinsics are
compiler-known functions, not `@c_extern` (with a use-site diagnostic and no function-value form); the
parser rule applies only in authorized units, with an Appendix C production and `>>` splitting; the
unconditional-conformance rule has extension-header and duplicate rules and amends §20.4.2; expanding
recursion covers methods, initializers, witnesses and signatures. Also folded in: the owner-needs-`deinit`
rule, `size_of` semantics for `Void` and zero-size types and bitwise relocatability, the `borrow_at` gap,
`Self` and static members, memberwise init, diagnostic precedence with §21, and the missing spec-change
list.

### Additional tests

- B2 at every context site, with `Box<Double>` from `1`, `nil`/`[]` arguments, an optional-wrapped
  expected type and a conflict; a generic callee's parameter providing no expected type.
- `extension Box<Int>: P`, `extension Box<T: Q>` and a wrong parameter count rejected; duplicate
  conformance across differently named headers.
- `Box<Box<Int>>` and `>>` parse; `joyeer_intrinsic_x < y > (z)` is an ordinary comparison in an ordinary
  program; intrinsics unauthorized and used as a value.
- A `Queue`-like type with a raw buffer: copy rejected with `deinit`, and the documented double-destroy
  without one is not claimed to be detected.
- `size_of`/`align_of` for `Void`, `Bool`, a function type, an enum with payload, an `indirect` enum, a
  nested generic struct, `String` and an array; identical at -O0 and -O2.
- Expanding recursion through a method, an initializer, a witness and a two-type cycle.
- A user generic type named like a builtin; a generic type's `deinit` running once, then its fields.
- Phase 4 smoke: queued `Void` and `[Int]` (constant-array clone) values in a runtime-source queue.

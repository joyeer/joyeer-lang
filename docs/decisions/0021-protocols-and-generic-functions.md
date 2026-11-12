# 0021: Protocols and bounded generic functions

- Status: accepted (2026-11-12), after a `language-designer` review whose findings are folded in below
  ("Review"). Phase 2 of the [generics plan](../plan/0010-generics.md); it settles that plan's open
  questions 1 to 3.
- Spec: [§20](../../specs/20-protocols.md) (normative for v1). The spec change also amends
  §1.4, §2.6, §2.9, §3.2.3, §4.1.1, §5.4, §12.2, §12.4, Appendix A and Appendix C (see "Spec
  changes").

## Context

[§20](../../specs/20-protocols.md) drafts protocols and generic top-level functions bounded by them,
dispatched statically by monomorphization. Nothing of it is implemented: `protocol` is reserved,
`generic_params` is rejected, and the only generic types are compiler builtins (`Array`, `Optional`,
`Result`, `Dict`, `JoinHandle`, the channel ends). Phase 4 of the plan needs user generics to move
those builtins into Joyeer; Phase 2 is the first step, generic *functions* only.

The draft leaves three questions open, recorded in plan 0010:

1. **Reviewability.** The preamble (§0.1) wants code a human can review. Monomorphization compiles one
   body many times; what keeps the behavior of every copy readable from the one body, and what does
   the compiler report so that the cost stays visible?
2. **Ownership of a type parameter.** Decision [0015](0015-noncopyable-and-linear-types.md) made
   copyability a property of a type and added linear types that must be consumed on every path. How
   does a generic body treat a `T` that may be noncopyable or linear?
3. **Where instantiation happens:** on the typed model after type checking, or during IR lowering;
   and how spans and debug scopes keep pointing at the generic source.

What the compiler does today, found by reading it:

- The typed model (`TypeCheckedModel` in `compiler/frontend/typechecking.h`) maps syntax node IDs to
  interned `TypeId`s, and records call targets and referenced symbols per node. One syntax tree, one
  type per node.
- The type checker reports `copy-of-noncopyable` at copy sites through `TypeCheckedModel::isCopyable`
  (0015, stage 4). Semantic analysis (`compiler/frontend/semanticanalysis.cpp`) runs once over the
  typed model: initialization, consumption, exclusivity and linearity.
- Lowering (`compiler/ir/irlowering.cpp`) reads `typeOf` and `callTarget` per node and emits one
  `ir::Function` per function symbol. `prepareOwned` emits a copy where a borrowed value is used as an
  owned one; destruction goes through per-type destroy helpers chosen by the backend.
- The IR verifier and the backend know only concrete types.

Constraints: zero-cost (§0.1.2), no GC and deterministic destruction (§4), and reliable authoring by
AI with human review (§0.1). Joyeer has no overloading (§12.2, §12.3, §13). Phase 1
([0020](0020-function-values.md)) showed that every new call form must flow through the exclusivity
check, the ownership paths of semantic analysis, and the verifier.

## Options considered

### A. What one generic body means (question 1)

1. **Template semantics** (C++): check each instantiation separately after substitution. Member lookup,
   copies and errors are resolved per instance, and errors appear inside the body only for some type
   arguments. Flexible, but the body no longer says what it does: a reviewer must know the type
   argument to know what a call does, and an instance can fail far from the call that caused it.
   Rejected.
2. **Checked once, resolved once** (Swift, Rust): check the body against its constraints only; every
   name, member, copy decision and diagnostic is fixed then. An instance differs from the body only in
   which witness a requirement call reaches and in how a `T` value is concretely moved, copied (only
   under `Copyable`) and destroyed. Both are named in the signature's constraints.

### B. Making instances visible (question 1)

1. **Nothing beyond `--emit-llvm`.** Instances are visible only by reading LLVM IR. Too indirect for a
   reviewer who wants to know what a change costs.
2. **A report flag.** The compiler prints, on request, every instance with its type arguments, why it
   exists, and its size in Joyeer IR instructions. Cheap, deterministic, and diffable in review.
3. **A hard budget** (warn or fail past N instances). No evidence yet for a number; deferred until
   Phase 2's measurement item (fern and the web server) gives one.

### C. A type parameter's ownership (question 2)

1. **Copyable by default**, opting out with `~Copyable` (Swift). Ergonomic for small values, but a copy
   of a heap-backed `T` can allocate and recurse (§4.1), and a body that copies would fail to compile
   only for `TcpConnection` or a `deinit` type, far from the body. It also adds a negative constraint
   form to the grammar.
2. **Noncopyable by default; `Copyable` opts in** (§20.5 as drafted). The body is checked as if `T`
   were noncopyable, so it compiles for every argument; copies are visible in the signature.
3. **Linear allowed as a type argument.** The body would have to be checked under 0015's linear rules
   (every `T` binding ended on every path, no drop). But generic code cannot *end* a `T`: only `join`
   and `consumeEmpty` end a linear value, and they are not requirements. Useful signatures
   (`func push<T>(value: consuming T)` storing into an array that is later dropped) would be rejected
   for every argument if checked linearly, or would drop a linear value if not. Rejected for v1. This
   does not reopen anything: 0015's "Not allowed in v1" list already excludes "a linear type as the
   type argument of a user type parameter".

### D. Where instantiation happens (question 3)

1. **Re-type-check per instance.** Substitute and run type checking and semantic analysis again for
   each type-argument tuple. This is option A1 in disguise: diagnostics could differ per instance, the
   typed model's one-type-per-node shape breaks (nodes are shared), and analysis cost grows with
   instances.
2. **Clone the syntax per instance.** Copy the function's syntax tree with fresh node IDs and type-check
   the copy. Keeps the model's shape but needs the "checked once" rule enforced by hand, and makes
   debug scopes point at nodes that no source file contains.
3. **Discover after analysis, substitute during lowering.** Type checking types a generic body once,
   with types that may mention type parameters (a new `TypeKind::typeParameter`). After semantic
   analysis, an instantiation pass walks from the non-generic code, collects every `(function, type
   arguments)` tuple reached and the witness of each requirement call. Lowering lowers each instance
   from the same syntax nodes, substituting concrete types and witnesses. Spans are the generic
   source's spans by construction; no node is copied. IR, verifier and backend see only concrete
   functions.

## Decision

Options A2, B2, C2 and D3, as the following rules.

### Meaning and reviewability

- **A generic function is checked once, and every resolution is made then.** Name and member lookup,
  literal types, copy and move decisions, exclusivity and the ownership checks of §4 are decided
  against the constraints, not against type arguments. No diagnostic is reported inside an instance
  except the instantiation backstop (below). There is no specialization: a generic function cannot
  behave differently for a particular type argument. Joyeer has no overloading, so a generic function's
  name follows the ordinary uniqueness and shadowing rules (§12.2, §13).
- **A type parameter is a distinct type.** Inside the body it equals only itself. Two different
  parameters, or a parameter and a concrete type, are never the same type, whatever the arguments
  later are. So postfix `?` requires the operand's error type and the enclosing function's error type
  to be the same type *in the body* (the same parameter or the same concrete type); §20.7's "same
  concrete type after instantiation" is replaced.
- **An instance differs from its body in two ways only:** a requirement call reaches the witness of the
  argument's conformance (§20.3), and a `T` value is moved, copied (only when `T: Copyable`) and
  destroyed with the concrete type's operation. Which moves, copies, accesses and destructions happen,
  and in what order, is fixed by the checked body. §20.6's "exactly like a hand-written function" and
  "ownership ... are those of the concrete types" are reworded to say this.
- **Instances are reported on request.** A driver flag prints, to standard output, one line per
  instance in a stable order (by qualified generic function name, then by the type arguments' display
  names): the instance's display name, what required it (`required by serveOnce<StaticFiles> at
  file:line`, or a non-generic function and its call site), and its size in Joyeer IR instructions
  before optimization, which is the same at every `-O` level and is not machine-code size. "What
  required it" is the requirer that is closest to a non-generic root, ties broken by least source
  position, so the line does not depend on walk order. Names are module-qualified where two private
  functions share a name. The report writes no output file. A byte-exact CLI fixture, run twice, proves
  it is stable. The flag's spelling, the display-name format and abbreviation of very long names are
  implementation details in [supported features](../impl/supported-features.md), not spec.
- **The cost model the reviewer reads is in the signature.** A requirement call is one direct call of
  the witness; copies of `T` exist only under `& Copyable`; the only per-instance cost is code size,
  which the report shows. A budget is deferred to the plan's measurement item. Whether CI or `fern`
  stores the report as a reviewable artifact is left to that item too.

### Ownership of a type parameter

- **`T` is noncopyable unless its constraint lists `Copyable`**, as §20.5 drafts. `isCopyable`,
  `isLinear` and "needs destruction" for a type-parameter type are answered from the parameter's
  constraint alone: copyable exactly when it lists `Copyable`, never linear, always possibly needing
  destruction. Because there is one interned type per parameter symbol, these stay context-free
  queries on `TypeCheckedModel`. The type checker reports `copy-of-noncopyable` at a copy site of `T`,
  with a note that `& Copyable` permits it; semantic analysis then checks the body once.
- **Types that mention a parameter follow §4.1.1.** Treat each parameter as copyable exactly when its
  constraint lists `Copyable`, then apply §4.1.1: `[T]` and `T?` are copyable when `T` is;
  `Result<T, TcpConnection>` never is; a function type is always copyable, whatever it mentions.
- **Lowering decides copy versus move from the unsubstituted type.** `prepareOwned`'s backstop asserts
  on a copy of any type that is noncopyable *in the body*, in every instance, including those where
  the argument is `Int`. Substitution only selects how a value is copied, moved and destroyed. So a
  copy site the type checker missed fails the same way for every instance, rather than copying for
  `Int` and failing for `TcpConnection`. Before generic code relies on it, Phase 2 verifies (and closes,
  if still open) the copy-site cases 0015 stage 4 listed as missing: the `?` operand and a `match`
  scrutinee.
- **A type argument of a generic function is never linear.** Inference that produces a linear type for
  a parameter is `type-checking.linear-type-argument` at the call, naming the parameter and giving
  0015's reason note. A type parameter is therefore affine: it may be dropped, and the instance
  destroys it at scope end with the concrete type's destruction (a `deinit` runs once, §4.7). The rule
  is scoped to generic functions. Generic *types* that may hold linear values (a library `Array<T>`
  holding `JoinHandle<R>`, which 0015 allows today) need a way to end a value from generic code; that
  is an open question for Phase 3 and 4, recorded in the plan.
- **`Never` is not a type argument** (`type-checking.invalid-type-argument`), including when it is
  inferred, such as `E` from `Result<Int, Never>`. The backend reason is 0020's: a `Never` value has no
  representation, a `Never` parameter would lower to an unusable function, and `Result<T, Never>`
  would need a layout rule generics do not need yet. `Void`, function types and builtin containers
  are allowed. §2.9 gains the rule.
- **Builtins whose rules depend on the concrete type are closed to a parameter.** A parameter (or
  `Self` in a requirement) cannot be a `Dict` key type: keys are limited to `Int`, `Bool`, `UInt8` and
  `String` and no protocol states that in v1. `print`, operators, `==`, `as` and string interpolation
  are not available on `T` (§20.5). `[String: T].get` clones the value, so it needs `T: Copyable`, as
  0015 already requires for any noncopyable value type.

### Constraints and requirement calls

- **Entailment.** Inside a body, a type that mentions a parameter satisfies `Copyable` when the
  §4.1.1 rule above makes it copyable. It satisfies a user protocol `P` only if it is exactly a
  parameter whose constraint lists `P`; a type built from a parameter (`[T]`, `T?`) never does in v1,
  because builtin containers conform only as concrete instantiations (§20.3). A concrete type argument
  inside a body is checked against the conformances as at any call.
- **Two protocols, one requirement name.** If `<S: P & Q>` and both `P` and `Q` require a method with
  one name, identical signatures (after `Self` is `S`) resolve to the single witness; differing
  signatures are an error at the constraint, since witnesses are found by name and no type can have
  both. Two requirements with one name in one protocol are a duplicate declaration. A protocol
  repeated in one constraint (`P & P`) is an error.
- **A requirement call's effects come from the requirement**, with `Self` replaced by the parameter:
  the receiver effect (`borrowing`, `mutating` with `&`, `consuming`) and each parameter's access
  effect. Requirement calls go through `collectEvaluationAccesses`, the ownership paths of semantic
  analysis and the linear checks, as any call does; so do calls of generic functions (Phase 1's
  lesson).
- **Only declared methods are witnesses.** A compiler-known member of a builtin (`removeLast`, `get`,
  `append`) is an IR operation, not a function, and cannot witness a requirement; such a conformance
  reports `missing-witness` with a note saying why.

### Inference (refining §20.4)

- Type arguments are inferred from argument types by structural unification over the parameter types.
  Parameters may be bound to the caller's own type parameters. Argument values are then checked
  against the substituted parameter types with the ordinary rules.
- **Order (§20.4, consistent with §2.8):** arguments that are not literals are unified first; a literal
  then takes the type already inferred for its position, and only a still-undetermined literal takes
  its default type. So `pair(a: 1, b: 2.5)` with `a: T, b: T` gives `T = Double`. A literal is an
  integer, floating, string or boolean literal, optionally negated; `1 + 2` is an expression and has
  its usual type.
- **Arguments that carry no type** (`nil`, `[]`, `[:]`, `.Case(...)`) and **diverging arguments**
  (`Never`-typed) contribute nothing; if nothing else determines the parameter,
  `cannot-infer-type-parameter`.
- **Optional wrapping** (§2.4) is not applied during inference: an `Int` argument for a `T?` parameter
  infers nothing for `T` from that argument (the argument is then checked after substitution, where
  wrapping applies as usual). An omitted defaulted argument contributes nothing; its default is
  checked after substitution. A function-value argument for a `(T): R` parameter unifies its function
  type structurally, effects included.
- **After inference, every constraint must hold**; `unsatisfied-constraint` names the type argument,
  the parameter and the protocol, and for `Copyable` adds 0015's noncopyable reason note.

### Where instantiation happens, and termination

- **Type checking** adds `TypeKind::typeParameter` (one interned type per declared parameter symbol)
  and types the generic body once. A requirement call records the requirement as its target; a generic
  call records its inferred type arguments, which may mention the caller's parameters.
- **Expanding recursion is rejected once, statically.** The type checker builds the generic call
  graph: an edge for each call from a generic function to a generic function, labeled with its
  substitution. A cycle in which some parameter is mapped to a type that strictly contains a parameter
  of the cycle (`f<T>` calling `f<[T]>`, or `f` and `g` passing `T?` back and forth) is
  `type-checking.expanding-generic-recursion` at the call in the body, whether or not any root reaches
  it. This is the "expanding cycle" rule of the CLI's generics. With it, every reachable instance set
  is finite.
- **An instantiation pass** runs after semantic analysis, only when earlier stages reported no errors.
  It starts from every non-generic body that lowering emits (functions, methods, `deinit` bodies,
  witnesses), follows generic calls, substitutes, and records each distinct `(function symbol,
  concrete type arguments)` once, in a cache keyed by that pair (`TypeId`s are interned, so equality is
  exact). For each instance it records the substitution and the witness of each requirement call. A
  generic function no root reaches has no instance and no code, but its body is still checked and its
  errors reported.
- **Backstop.** The static rule makes growth finite but not small (a non-cyclic chain of generics can
  still nest types deeply). The pass also caps the total instance count, the nesting depth of a type
  argument, and the depth of the shortest chain from a root. Exceeding one is
  `type-checking.instantiation-limit` (kept under the `type-checking` prefix because it is a frontend
  diagnostic about the program's types, reported before lowering) at the requiring call, with notes
  for the first and last steps of the chain. The caps are implementation-defined and documented in
  supported features; §20.6 keeps only the floor of 32 for depth.
- **Lowering** lowers each instance as its own `ir::Function`, reading the same syntax nodes, applying
  the instance's substitution to every type it emits and its witness map to every requirement call,
  with copy decisions made as above. Lowering never emits a type that contains a type parameter.
- **IR, verifier and backend see only concrete functions.** The verifier rejects any type that contains
  a type parameter (defence in depth). The IR gains no opcode. An `ir::Function` carries the instance's
  display name as a string, which the existing wire format already records. No wire layout or
  version change is needed.
- **Spans and debug scopes point at the generic source.** Every instruction of an instance carries the
  generic body's spans. Each instance has its own function debug scope at the generic declaration,
  named with the instance's display name (`totalArea<Square>`), with locals at their concrete types.
  Linkage names are internal, unspecified and deterministic. Two caveats are documented, not
  prevented: identical-code folding (`/OPT:ICF`, LLVM mergefunc) can merge two instances so a debugger
  shows one name, and a breakpoint by bare name (`break totalArea`) may need the debugger's
  regular-expression form; a test checks the latter on one debugger.
- An internal error in an instance (a lowering or verifier failure, which the checks above should make
  impossible) is reported with a note naming the instance and what required it.

### Spec changes

- §20: made normative for v1, with the rules above that are language rules: type parameters are
  distinct types, the `?` rule, inference order and the no-information arguments, entailment, two
  protocols with one requirement name, only declared methods witness, the linear and `Never` bans,
  the closed builtins, the expanding-recursion rule, and the reworded §20.6. §20.4 drops "two generic
  functions cannot be overloaded" (no overloading exists) and "unify to one concrete type" (a caller's
  parameter is allowed). §20.5's "free functions may be called with `T`" becomes "functions whose
  parameter types accept `T`: generic functions, and function values whose type mentions `T`".
  §20.8 gains `linear-type-argument`, `invalid-type-argument` and `expanding-generic-recursion`.
- §1.4: `protocol` moves from reserved to declarations. Appendix A follows §1.4.
- §2.6 and §3.2.3: generic top-level functions are accepted; generic types and methods stay deferred.
- §2.9: `Never` is not a type argument.
- §4.1.1: type parameters and `Copyable`; the linear paragraph notes the generic-function ban.
- §5.4: member calls on a type parameter are requirement calls.
- §12.2: constraint protocols are exposed names of a public generic signature. §12.4: `Copyable` is a
  prelude name.
- Appendix C: `protocol_decl`, `generic_params`, and the conformance list. The extended type of a
  conformance extension is restricted to a nominal type with optional type arguments, an array type or
  a dictionary type, not a function type, which keeps `extension (Int): Int: P` out of the grammar.

## Consequences

- Easier: a reviewer reads one body and knows what every instance does; the signature shows the cost
  model; the report shows instance count and size in a diffable form.
- Easier: the IR, the verifier and the backend need no generic concepts; no new opcode, and the backend
  change is the instance name in debug information.
- Harder: lowering carries a substitution and a witness map; every type lowering emits must go through
  substitution. A missed one is caught by the verifier's type-parameter rule, not miscompiled.
- Harder: `T` is noncopyable unless stated, so some generic code needs `& Copyable` where another
  language would not. This is intended (§20.5).
- Not possible in v1: a linear or `Never` type argument, specialization, return-type-only inference,
  a parameter as a dictionary key, overloading. Phase 4's `JoinHandle<R>` is a linear generic *type*,
  not a linear type argument, so this rule does not block Phase 4 itself; generic types that hold
  linear values are Phase 3's question.

### Tests

- Parser: protocols, requirements with effects, conformance lists, `<T: P & Q>`, the restricted
  extended type, `P & P`.
- Name resolution: parameter scope, `Self` in protocols, a protocol name used as a type, a reserved
  protocol name, `Copyable` as a parameter name.
- Type checking: each witness mismatch, a builtin member as witness, orphan and duplicate conformances,
  two protocols with one requirement name; type parameters are distinct (`var x = a; x = b` with
  `a: T, b: U`, and `r?` with `Result<Int, E>` in a function returning `Result<Int, String>`, rejected
  even if only ever called with `String`); each inference rule (literal order, negated literals,
  `nil`/`[]`/`.Case`, `Never` arguments, defaults, optional wrapping, function-value arguments); each
  inference failure; entailment inside a body; `Copyable` with a `deinit` type; linear and `Never`
  type arguments; `Void` and function types as type arguments; a parameter as a dictionary key; `print`
  of a `T`; `get` with and without `Copyable`; a private type as a type argument; generic native
  bindings; expanding recursion through one and through two functions.
- Semantic analysis: copy of a noncopyable `T`; consuming, `mutating`, `inout` and `initializing`
  requirement calls with `Self`; exclusivity through requirement and generic calls.
- Instantiation: cache hits; transitive instances; a generic called from a method and from a `deinit`;
  a generic no root reaches (no code, errors still reported); the backstop (a type argument that
  doubles per level, and the two-branch case) with output identical under any declaration order.
- Lowering, IR and backend: substituted types, witness calls, the copy backstop firing for `T = Int` as
  well, verifier rejection of a type parameter, wire round trip, debug names.
- CLI: the instantiation report, byte-exact and stable across two runs.
- End to end (`tests/e2e/`): a `deinit` type through a `consuming` requirement (its `deinit` runs
  exactly once) and a dropped `T` destroyed once, a `Copyable` bound, a `mutating` requirement, a
  generic calling a generic, a generic in a dependency module instantiated with a root-module type and
  witness; run at -O0 and -O2 with the same output.

### Diagnostics

Those of §20.8, plus `type-checking.linear-type-argument`, `type-checking.invalid-type-argument` and
`type-checking.expanding-generic-recursion`.

## Open questions

1. Should CI or `fern` record the instantiation report as a reviewable file, so instance growth shows
   in review without asking for it? Decided with the plan's measurement item.
2. How generic code ends a linear value, needed before generic types may hold one (Phase 3 and 4).

## Implementation notes

The implementation starts in dd059d9, with completion work tracked in plan
0010. The original implementation was work in progress, not
full-suite validated or implementation-reviewed.

- Instance discovery runs at the end of successful type checking, before
  semantic analysis, rather than in a separate post-analysis pass. Semantic
  analysis still checks each body once, and must succeed before lowering or
  an instantiation report is produced. Discovery is bounded independently.
- The wire version stays 15: the existing function `name` field transports
  each instance's display/debug name, so no additional field is required.
- Source-level copies are checked against the unsubstituted generic type.
  The lowering backstop only sees resource-owning values: for a trivial
  `Int`, ownership acquisition is a no-op, so `prepareOwned` does not
  distinguish a logical copy from a transfer. This is narrower than the
  originally proposed backstop for every type argument; it does not relax
  the type checker's `Copyable` rule.
- The §20.4 `totalArea` example uses an index loop. `for-in` copies its
  sequence/elements and would require `S: Copyable`; the index loop borrows.
- `--report-instantiations` uses the format and fixed limits documented in
  [supported features](../impl/supported-features.md). The report counts
  unoptimized Joyeer IR instructions, not machine-code bytes.
- Function parameter defaults are not implemented for ordinary or generic
  functions; the parser has no parameter-initializer representation. The
  inference rule for omitted defaults describes the design, not an accepted
  source form in this implementation. Adding general default arguments is
  not part of Phase 2.

## Review

`language-designer` (2026-11-12) found three blockers, all folded in: §20.7's `?` rule was a
per-instance check (now: type parameters are distinct types); a depth limit alone did not make
instantiation terminate (now: the static expanding-recursion rule plus caps); and lowering's copy
backstop judged the concrete type (now: the unsubstituted type). Its should-fix items are folded in
too: no overloading exists; copyability of types that mention a parameter, including function types;
dictionary keys, `print` and `get` on `T`; inference order and the no-information arguments; two
protocols with one requirement name; builtin members as witnesses; the copy check belongs to the type
checker; §20.6's wording; a well-defined "required by"; the scope of the linear ban; the `Never` ban's
scope and reason; a list of spec changes; and the missing tests. Nits on diagnostic prefixes, debug
names and folding, the wire field, §20.5's free-function sentence and the grammar of the extended type
are folded in as well.

The independent implementation reviews found nine distinct issues
(`type-system-reviewer`: eight, `ir-backend-reviewer`: two, with propagation
reported by both). Each was reproduced by a failing GoogleTest and fixed:

- propagation copied a borrowed generic operand only when the result entered
  an owned context; the operand is now checked for discarded and borrowed
  uses too, and lowering uses its span for the abstract-copy backstop;
- inferred and nominal dictionary keys bypassed the opaque-key restriction;
- nonliteral inference inputs could be retyped by an earlier argument;
- parentheses caused context-free arguments to be checked prematurely;
- a chained `Self` result lost the caller's other protocol constraints;
- unused generic bodies accepted bare type/protocol names as values;
- witness visibility ignored private-file boundaries;
- protocol references silently ignored type arguments;
- used error placeholders, including nested ones, escaped IR verification.

Completion tests also caught public signatures treating their own type
parameters as inaccessible, malformed parameter clauses, nested `Never`
inference, incorrect report reasons, missing instantiation-chain limits and
protocol-name receiver calls. Instance-name disambiguation preserves
ordinary nominal debug names and distinguishes types shadowing prelude
names. Type-as-value checks cover qualified names too, and propagation
retains copy diagnostics even in an invalid return context.
`generics_propagation.joyeer` covers consumed
success/error results with strings and deinit-traced values, and a chained
`Self` result across two constraints, at `-O0` through `-O3`.

The reviewers inspected source; the author ran the reproductions and
regressions. The independent `adversarial-tester` invocation was rejected by
the service (HTTP 422) before executing, so that review remains unrun.

## Validation and measurements

See the [local timing/footprint sample](../impl/backend.md#phase-2-generics-completion-sample)
for fern and the web server. Their generated code and executable sizes are
unchanged in this sample; neither currently instantiates user generics.
No automatic report archiving or additional cost budget is added beyond the
documented implementation safety limits.

Full-platform acceptance remains open. `python scripts/validate.py
--llvm-root C:\LLVM-22.1.8` exits 9009 because Python is not installed.
The final Windows x64 Debug default build succeeds, and unfiltered CTest
passes **1591 of 1591**. The report fixture is byte-exact across two runs at
both `-O0` and `-O2`; the final fern/web-server reports each contain zero
instances.
The manually run Debug root `fern test` has six passing modules and one
pre-existing `SystemTests` failure: read-only stderr reports error 13, and
a zero-byte write to a closed descriptor reports success. The same failure
reproduces with the `3feb624` baseline compiler. It is not changed here.
Release-host and Linux acceptance, the independent adversarial review, and
the debugger breakpoint check remain unrun; no debugger is available.

## Evidence

- Swift and Rust check generic bodies once against their bounds; C++ templates do not, and their
  instance-time errors are a known cost for reviewers and code generators alike.
- The ECMA-335 CLI rejects expanding generic cycles statically for the same termination reason.
- Rust's `Copy`/`Clone` split and Swift's later `~Copyable` show that copyability must be visible to
  generic code; making it opt-in matches §4.1's rule that copies of heap-backed values are not free.
- 0015 already listed a linear type argument as not allowed in v1.
- 0020's implementation review found a use-after-free where a new call form missed the exclusivity
  and ownership paths; this record names those paths for requirement and generic calls up front.

## Adversarial review findings (2026-11-12)

`adversarial-tester` found no crash and no wrong code. Fixed: an inference failure no longer cascades into
a function-type mismatch, and `func main<T>()` is `invalid-generic-declaration`. Open, all low severity:

- A literal beside an already-inferred `T?` takes its default type and conflicts (`two(a: 1, b: o)` with
  `o: Int?`); §20.4.1 does not say literals wrap.
- A user `struct` or `enum` may be named `Copyable`, `Equatable` or `Display`; only a `protocol` of those
  names is rejected (§20.2).
- A method, requirement or label named `Self` is accepted, though a type parameter `Self` is rejected.

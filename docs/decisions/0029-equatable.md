# 0029: Equality through `Equatable`

- Status: proposed (2026-11-21). The user chose explicit opt-in, the requirement `equals(other:)`, `x == nil` as a
  presence test on any optional, and conformance for noncopyable but not linear types (2026-11-21). The other answers
  are the author's, revised after a language-designer and a type-system review (see "Review"). Work is tracked in
  [plan 0012](../plan/0012-equatable.md).
- Spec, once accepted: §2.1.2, §2.4, §2.5, §2.6, §5.2, §12.4, §20.2, §20.3, §20.4.2, §20.5, §20.6, §20.8, §20.10,
  §20.12.2, §20.12.5, a new §20.13 and Appendix C. [0024](0024-library-optional-and-result.md) is amended (below).
- Builds on [0021](0021-protocols-and-generic-functions.md) (protocols), [0022](0022-generic-types-and-layout-intrinsics.md)
  (generic types), [0024](0024-library-optional-and-result.md) (which rejects `==` on `Optional` and `Result`) and
  [0026](0026-library-array.md) (library `Array`).

## Context

`==` and `!=` are decided by a type-kind test with no basis in the spec. `binaryResult` (`typechecking.cpp`)
accepts them when both sides have one type that is not `Void`, is copyable, contains no function type, mentions no
type parameter, and is neither a struct nor an enum. After 0024 and 0026 that leaves exactly the integer types,
`Bool`, `UInt8`, `Float`, `Double` and `String`, but nothing in the language says so:

- user structs and enums, `Optional`, `Result` and arrays have no equality at all, so `p == q` on a two-field point,
  `x == nil` and `c == .Red` on a payload-free enum are rejected and need a `match`;
- generic code cannot compare a `T` (§20.5), so a library cannot write `contains` or `indexOf`;
- `Dict` keys are closed to four types because a library cannot compare keys (0027).

§20.10 item 2 reserves `Equatable` for this. The constraints are the project's own: no hidden cost on scalar `==`
(§0.1.2), a reviewer can see which types have equality (AI writes, humans review), no overloading (§12.2, §13), the
coherence of §20.3, and generic bodies that are checked once (§20.6).

## Options considered

1. **Operator functions** (`func ==(left: Point, right: Point): Bool`). Needs operator declarations and overloading
   on the operator's name, which Joyeer rejects (§12.2, §13, §20.10 out of scope). A generic `T` would still need a
   protocol to say that `==` exists for it. Rejected.
2. **Implicit structural equality for every type** whose fields have it. No boilerplate, but `==` appears on types
   whose authors never meant it (a file descriptor wrapper, a generated ID), and adding a non-comparable field to a
   type silently removes `==` from every caller. Generic code would still need a constraint. Rejected by the user.
3. **A compiler-known protocol with explicit, derivable conformances.** `==` is defined through one protocol;
   builtin types conform by the compiler; a user type conforms only by declaring it, and an empty conformance derives
   the witness. Chosen.

Within option 3, two further choices were made:

- **`x == nil` as a presence test** (chosen by the user) rather than ordinary `Optional` equality, which would need
  the payload to conform and so reject `f == nil` on a function optional, the most common optional test.
- **Derived witnesses are not members** (from review) rather than generated `equals` methods. A generated method on
  `Box<T>` would exist for every `T` (§20.12), so `b.equals(other: c)` on a `Box<(Int): Int>` would type-check and
  compare functions; it would also give `Point` a member that `Int` and `[Int]` lack.

## Decision

### The protocol

The role prelude (`lib/Runtime/roles.joyeer`) declares

```joyeer
@compiler_role(equatable)
public protocol Equatable {
    func equals(other: Self): Bool
}
```

The compiler binds `==` to the protocol by its role, never by its name. The receiver and the argument are borrowed
(the defaults, §3.2.4, §4.2). `Equatable` may now be named in constraints and conformance lists. The name is reserved
in the whole type namespace: a user `protocol`, `struct`, `enum` or type parameter named `Equatable` is
`type-checking.reserved-protocol-name`, so the name in a constraint always means this protocol. This rejects a user
`struct Equatable`, which 0021 accepted; no fixture or library declares one. The role prelude is exempt.

### What `==` means

For two operands of one type `X`:

- `a == b` type-checks exactly when `X` conforms to `Equatable` (below), and is true when `X`'s witness returns true;
- `a != b` is the negation of `a == b`; there is no separate `!=` requirement, so the two never disagree;
- otherwise `type-checking.invalid-operator-operands`, with help naming `Equatable` and, for a user type of the current
  module without a conformance, the extension to add (`opt == 5` keeps its rejection, with help suggesting `.Some(5)`).

**Operand typing.** The operands of `==` and `!=` are not given the enclosing expression's expected type. An operand
that has a type of its own is checked first; an operand without one (`nil`, an implicit member such as `.Red`, an
array or dictionary literal, a numeric literal) is then checked against that type, on either side. If neither has a
type of its own, it is `missing-contextual-type` (`.Red == .Red`). So `c == .Red`, `.Red == c` and `[] == xs` are
accepted. There is no conversion or implicit wrapping between the operands.

**Evaluation and access.** The left operand is evaluated, then the right, and the result is computed. The access
rules depend on the conformer:

- For a **builtin conformer** (below), the left operand is read as a value before the right is evaluated, as today.
  `n == bump(&n)` on an `Int` stays accepted and compares the old value.
- For **every other conformer**, and for any `==` in a generic body, `a == b` is analyzed exactly like the call
  `a.equals(other: b)`: the left operand is a borrow that stays active while the right is evaluated (§4.4). So
  `p == bump(&p)` is `overlapping-access`, and `a == reopen(t: consume a)` is a use after move. `a == a` is two shared
  borrows and is accepted.

### Which types conform

1. **Builtin conformers** conform by the compiler: every integer type, `Bool`, `UInt8`, `Float`, `Double`, `String`,
   and `Void`, whose values are all equal (it is needed so that `Void?` and `Result<Void, E>` conform). `Char`
   conforms when it is implemented. A user conformance of a builtin type is `orphan-conformance`, as today (§20.3).
   Their lowering is unchanged: the scalar `compare` opcode and the inlined `string_equal` prelude call (0028 stage 3).
2. **`Float` and `Double` keep IEEE equality**: `NaN != NaN` and `-0.0 == 0.0`. `Equatable` states no law that the
   compiler checks. Derived equality and `array_equal` compare floating-point fields one by one and never take an
   identity shortcut, so `a == a` is false when `a` holds a NaN. A future `Hashable` must exclude floats or define
   NaN's hash.
3. **A user `struct` or `enum` conforms by `extension X: Equatable { ... }`** in its own module (§20.3). If the type or
   any of its extensions declares a member named `equals`, the conformance is **hand-written**: that member is the
   witness and is checked as any witness (`witness-mismatch` for `equals(to:)` or a wrong type). Otherwise the
   conformance is **derived**:
   - a struct compares its stored fields in declaration order, stopping at the first that differs; a struct with no
     stored fields is always equal;
   - an enum compares tags, then, for equal tags, the payloads in order, stopping at the first that differs;
   - every stored field and payload type must conform. A `Never` payload counts as conforming, since no value of it
     exists;
   - the derived witness is not a member: `p.equals(other: q)` is an unknown member unless `equals` is declared. Write
     `==`. This holds for every non-hand-written witness, builtin and array ones included, so `equals` exists on a
     concrete type exactly when its author declared it.
4. **A generic user type** with a derived conformance (`extension Box<T>: Equatable {}`) conforms **structurally and
   conditionally**: `Box<X>` conforms when every field and payload type, with `X` substituted, conforms. This is the
   one exception to the unconditional rule of §20.12.2, limited to derived `Equatable`; general conditional
   conformance and `where` clauses stay deferred (§20.10 item 1). A hand-written `equals` on a generic type stays
   unconditional and is checked once against the type's declared constraints, as today.
5. **`Optional` and `Result`** conform the same way. The role prelude declares `extension Optional<T>: Equatable {}`
   and `extension Result<T, E>: Equatable {}`, so `X?` conforms when `X` does and `Result<X, Y>` when both do. This
   amends 0024: these two extensions, with empty bodies, are the one conformance its shape check admits on the role
   enums; methods stay rejected. §20.3's "not an optional" stays for user extensions.
6. **Arrays**: `[X]` conforms when `X` does. An `array_type` struct cannot be extended, and its fields are a handle, so
   derivation would compare addresses. The collections prelude declares one role-bound generic function,
   `@compiler_role(array_equal) private func arrayEqual<T: Equatable>(left: [T], right: [T]): Bool`, which compares
   counts and then elements in order with `==`, stopping at the first difference. Its instance is the witness of
   `[X]`. It needs no conditional-conformance machinery and no runtime element-compare callback, and it is inlinable
   like any prelude function.
7. **`Dict` does not conform.** Order-independent equality needs key lookup per entry, and the key rules are still
   closed (§2.6). Dictionary equality and broader keys wait for a `Hashable` decision.
8. **Never conforming**: a function type (identity is not stable across optimization, §2.3.1); a `Dict`; a linear
   type, including one that is linear because of a field or payload (below). A struct with a function or `Dict` field
   may still conform with a hand-written `equals`; it cannot derive.

`match` patterns are structural and need no `Equatable`; a literal pattern on a `String` or scalar is unchanged.

### Conditional conformance: checking and termination

A derived conformance is recorded with its structural predicate, not as an unconditional fact. The predicate is
evaluated wherever conformance is asked: at `==`, in constraint satisfaction (§20.4.2) and at generic calls.

- **Order.** All conformance declarations are collected before any predicate is evaluated, so the order of
  extensions, within a file or across files, does not matter.
- **Termination.** The check is coinductive: while `X: Equatable` is being checked, a nested query for the same `X`
  holds. `indirect enum List { Nil, Cons(Int, List) }`, `struct Node { var kids: [Node] }` and mutually recursive
  generic types therefore terminate. A provisional answer is not cached, as with linearity (§4.1.1). Types whose
  arguments grow are already rejected by §20.12.4.
- **Generic bodies are still checked once.** Inside a generic body, a type parameter satisfies the predicate exactly
  when its constraint lists `Equatable`. If the predicate holds for `T` under its constraints, it holds for every `X`
  that satisfies them, so no instance can fail. `func f<T>(a: T?, b: T?): Bool { a == b }` is rejected in the body;
  with `<T: Equatable>` it is accepted.
- **The derived witness has no checked body.** The compiler generates it per instance, only for an instance whose
  predicate has passed. This is a generated operation, like copy and destroy, not a specialization (§20.6).
- **Diagnostics.** A field or payload whose type can never conform under any substitution (a function type, a
  `Dict`, a linear type, a non-conforming concrete type) is `type-checking.non-equatable-member` at the extension,
  naming the field or the case and payload position, with a note when it is a `Dict`. A field that conforms only for
  some arguments (`var value: T`) is checked at use: `Box<Fn> == Box<Fn>` is `non-equatable-member` naming the field
  path, with a note pointing at the extension; a generic call that infers such a type for `U: Equatable` is
  `unsatisfied-constraint` with the same field-path note, as `Copyable` notes already explain a noncopyable type.

### Ownership

Equality only borrows, so a **noncopyable** type (one with `deinit`, or holding a noncopyable field) may conform, and
`==` on it borrows both sides. Today's copyability requirement on `==` goes away. A **linear** type may not conform:
its conformance is `type-checking.invalid-conformance`, reported instead of any `non-equatable-member` for the same
extension. Type parameters are never linear (§20.5), so generic and concrete equality cover the same types, and a
must-consume resource has no meaningful value equality.

### Presence test: `x == nil`

When one operand is the absent case of the `optional` role without a payload, written `nil` or `.None` (on either
side, also in parentheses), and the other has an optional type `X?`, `==` and `!=` test the other operand's tag. The
test is valid for every optional, whatever `X` is (a function, a noncopyable or linear type), never calls a witness,
borrows the operand, and costs one tag comparison. The form is recognized from the syntax before ordinary equality,
so `f == .None` on a function optional is a presence test too. `nil == nil` is `missing-contextual-type`. A linear
operand must be a place: `spawnMaybe() == nil` borrows and then drops a linear temporary, which is
`semantic-analysis.linear-result-dropped`.

### Generic code

Inside a generic function or type, `==` and `!=` on operands of a type that mentions type parameters type-check when
the type satisfies `Equatable` by the predicate above. §20.4.2 is extended for this one protocol, so `[T]`, `T?`,
`Result<T, E>` and a `Box<T>` with a derived conformance satisfy `Equatable` when their components do. A requirement
call `a.equals(other: b)` on a `T: Equatable` is allowed (§20.5 item 1), and in each instance it calls the same
witness `==` would. Without the constraint, `==` on `T` is `invalid-operator-operands` with a note that suggests adding
`Equatable` to `T`'s constraint.

### Witnesses and lowering

The type-checked model records a witness kind for each conforming type: a builtin operation (the `compare` opcode or
the `string_equal` role call), a hand-written method, a derived witness, or an `array_equal` instance.

- Builtin conformers lower exactly as today, so scalar and `String` code is unchanged in size and speed.
- A hand-written or derived conformance lowers `a == b` to a direct call of its witness; `!=` adds `compare eq` with
  `false`, as `String` `!=` does now. A derived witness is an ordinary generated function, a candidate for inlining.
- `[X] == [X]` calls the `array_equal` instance; `X?` and `Result` use their derived witnesses (tag, then payload).
- There is no table, no boxing and no dynamic dispatch, and no IR opcode is added. The cost of `==` is the cost of its
  witness.

### Prelude and grammar

`protocol_decl` takes no attributes, so Appendix C gains `prelude_protocol` (compiler-bundled source only, like
`prelude_struct`), and §20.12.5 lists the two new roles: `equatable` on a protocol and `array_equal` on a generic
function.

## Consequences

- `==` has one rule for every type, and generic code can compare. `c == .Red` on a payload-free enum needs one line,
  `extension Color: Equatable {}`.
- Every program accepted today that compares with `==` keeps its meaning and code. Newly accepted: `Void == Void`,
  `==` on noncopyable conformers, `nil == x` and `.Red == c`, and everything the conformances add. Newly rejected: a
  user type or type parameter named `Equatable`.
- The type checker gains conformance lookup for a compiler-known protocol, builtin conformances, conditional derived
  conformances with a coinductive predicate, the §20.4.2 extension, symmetric operand typing, the presence test and
  witness kinds. Semantic analysis gains the borrow rule for non-builtin `==` and the linear-temporary check.
- Lowering gains generated witnesses for structs and enums, recursive ones included, witness calls for `==`, and the
  `array_equal` call.
- Diagnostics: `invalid-operator-operands` keeps its ID with new help and notes; new
  `type-checking.non-equatable-member`; `invalid-conformance` covers a linear conformer; `reserved-protocol-name`
  widens to every type-namespace declaration of `Equatable`; `unsatisfied-constraint` gains a field-path note.
- Tests (type-checking fixtures, GoogleTests and end-to-end fixtures):
  - accepted and rejected `==` on every builtin conformer, `Void == Void`, NaN fields;
  - structs, payload enums, `indirect` enums, `[Node]` recursion, mutually recursive generic types, extensions in
    either order and across files;
  - `Box<Int>` accepted, `Box<(Int): Int>` rejected at `==` and at a generic call, a `Dict` field rejected at the
    extension, a linear enum reporting only `invalid-conformance`;
  - `equals(to:)` beside an empty conformance, a wrong-shaped `equals`, a field named `equals`, `p.equals` on a
    derived conformer, `extension Int: Equatable {}`;
  - `Int?`, `Point?`, `Result<Int, String>`, `Result<Int, Never>`, `[Int]`, `[[String]]`;
  - `nil == x`, `x != nil`, `f == .None` on a function optional, `.Red == c`, `[] == xs`, `.Red == .Red`,
    `spawnMaybe() == nil`;
  - `p == bump(&p)` on `Point` (rejected) and `n == bump(&n)` on `Int` (accepted), `a == consume a`, `a == a` on a
    noncopyable conformer;
  - generic `==` on `T` with and without the constraint, unconstrained `T?` equality, `t.equals` instantiated with
    `Int`, `String` and `[Int]`, `contains<[Int]>` and `contains<Int?>`;
  - a user `struct Equatable` and a type parameter named `Equatable`.
- Cost gate: scalar and `String` `==` unchanged in size and speed at -O0 and -O2; array and derived struct equality
  benchmarked against hand-written loops and field comparisons.

## Review

Both reviews ran on 2026-11-21, separately from the author, on the first draft. Neither reopened the user's four
choices.

- **language-designer:** two high findings, both addressed. The derived witness as a generated method was unsound for
  generic types and uneven across conformers; it is no longer a member. The spec list was incomplete; it now names
  §2.1.2, §2.4, §2.5, §20.3, §20.12.2, §20.12.5 and Appendix C, and this record amends 0024. Medium findings addressed:
  reserving the name in the type namespace, symmetric operand typing, `.None` as a presence test, the borrow rule,
  selection of a hand-written witness by member name, coinductive termination, and witness kinds. Low findings
  addressed: `Char`, rejection at the extension, diagnostic precedence for linear types, `match`, `Hashable` and NaN,
  the reason for `Void`, and the `Dict` note.
- **type-system-reviewer:** one critical finding, addressed: borrowing the left operand across the right without a
  rule allowed a use after move on noncopyable conformers. `==` on non-builtin conformers is now analyzed as the
  `equals` call, and builtins keep today's read. High: conditional conformance on today's unconditional `satisfies`
  would fail at instantiation; the predicate is now recorded and evaluated, generic bodies use the parameter's
  constraint, and the derived witness is generated only after the predicate passes. Medium findings addressed:
  termination without caching provisional answers, collecting conformances before deriving, where each failure is
  reported, operand typing, linear temporaries, and witness kinds. Low findings addressed: function and linear
  fields, NaN, the reserved-name exemption, and help text.

## Evidence

- 0024 measured today's rule: `Point?` was accepted although `Point == Point` was rejected, and no fixture compared
  optionals, which failed in the backend.
- 0027 rejected a generic `Dict` over `K: Equatable` because `Equatable` did not exist; this decision does not change
  that choice, only enables a later `Hashable` one.
- Swift's synthesized `Equatable` and Rust's `#[derive(PartialEq)]` both require an explicit opt-in for the same review
  reason, and both keep IEEE semantics for floats.

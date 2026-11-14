# 0012: Equality through `Equatable`

- Status: proposed (updated 2026-11-14)
- Decisions: [0021](../decisions/0021-protocols-and-generic-functions.md),
  [0022](../decisions/0022-generic-types-and-layout-intrinsics.md),
  [0024](../decisions/0024-library-optional-and-result.md) (rejects `==` on `Optional`/`Result`);
  a new decision record is the first todo item
- Spec: [§5.2](../../specs/05-expressions.md), [§12.4](../../specs/12-modules.md) (reserved names),
  [§20.5](../../specs/20-protocols.md), [§20.10](../../specs/20-protocols.md) item 2

## Goal

Define `==` and `!=` through the reserved `Equatable` protocol (§20.10 item 2), so equality has one
rule for every type. Scalars and strings keep their current behavior. `Optional`, `Result`, arrays,
user structs and user enums get equality when their contents have it. Generic code can compare a
`T: Equatable`. Today, equality is decided by a type-kind test with no spec basis, and it accepts
types it cannot compile (0024, "Equality is rejected").

## Definition of done

- `Equatable` is declarable as a constraint and as a conformance; it is no longer only a reserved
  name.
- `a == b` and `a != b` type-check exactly when both sides have one type that conforms to `Equatable`.
  They lower to the conformance's witness, or to the existing scalar and string comparisons, with no
  extra call for builtin scalars.
- `Int?`, `Result<Int, String>`, `[Int]`, and a user struct or enum with an explicit conformance
  compare correctly in end-to-end fixtures, in Debug and Release.
- `x == nil` and `x != nil` work on any optional, as a tag test that needs no payload equality.
- Inside `func f<T: Equatable>`, `==` on `T` is accepted. Without the constraint, it is rejected with
  a note that suggests adding it.
- Floating-point equality keeps IEEE semantics (`NaN != NaN`), stated in the spec.
- Function types and types containing them never conform.
- Separate role reviews and full Debug/Release platform validation are recorded with evidence.

## Current state

- **Type checking:** `binaryResult` (`typechecking.cpp`, `equal`/`notEqual`) accepts `==` when both
  sides have the same type and that type:
  - is not `Void`, is copyable, contains no function type, and mentions no type parameter;
  - is not a user enum or struct.
- **Lowering:** `Builder::comparison` (`native_functions.cpp`) handles only:
  - floating point (`FCMP_OEQ`/`UNE`);
  - `joyeer.string` (`joyeer_string_equal_abi`);
  - integers (`ICMP_EQ`/`NE`).

  So `[Int] == [Int]`, `[Int: Int] == …` and optionals are accepted but do not lower. No fixture
  compares them.
- **After 0024:** `Optional` and `Result` reject `==`, with help pointing to `match`.
- **Spec:** `Equatable` is reserved and unusable (§12.4, §20.2, `type-checking.reserved-protocol-name`).
  Generic code cannot use operators on `T` (§20.5).
- **Gaps:** there are no default implementations, conditional conformances or operator declarations
  (§20.10). Conformances of builtin types and conformances outside the type's module are not
  implemented (§3.5).

## Todo

### Phase 1: decision

- [ ] Record a decision for `Equatable`. It must settle the open questions below and state how each
  rejected option fails (operator functions, implicit structural equality for every type).
- [ ] Run the language-designer and type-system reviewers on it; record the outcome in the decision.
- [ ] Amend §5.2, §12.4, §20.2, §20.5, §20.10 and Appendix C. Add a §20 subsection for `Equatable`
  with its diagnostics.

### Phase 2: builtin types and the operator

- [ ] Write failing tests:
  - type-checking stage fixtures for every accepted and rejected `==` form, including those that
    0024 rejects today;
  - GoogleTests for conformance lookup and the scalar fast path.
- [ ] Make `Equatable` a compiler-known protocol bound by a closed prelude role, like 0024, not by
  its name. State its requirement.
- [ ] Give builtin conformances to the integer types, `Bool`, `UInt8`, `Float`, `Double` and `String`.
  Lower them through the existing `comparison` paths so code size and speed do not change.
- [ ] Replace the kind test in `binaryResult` with conformance lookup. Keep `invalid-operator-operands`
  and add help naming `Equatable`.
- [ ] Accept `==` on `T` when the constraint lists `Equatable` (§20.5). Lower it to the witness after
  monomorphization.

### Phase 3: derived conformances

- [ ] Accept `extension S: Equatable {}` with no body on a user struct or enum, and derive memberwise
  (or case-then-payload) equality. Reject it when a field or payload is not `Equatable`, naming the
  field.
- [ ] Accept a hand-written `equals` witness in place of the derived one.
- [ ] For a generic type, define when `Box<X>` conforms: the derived conformance holds when every
  field type, with `X` substituted, conforms. Check it at use, and record the rule in the decision.
- [ ] End-to-end fixtures: compare structs, payload enums, recursive `indirect` enums, and generic
  instances; reject the non-conforming ones.

### Phase 4: Optional, Result and containers

- [ ] Give the `optional` and `result` role enums the derived, payload-conditional conformance.
  Remove 0024's rejection and its `match` help.
- [ ] Lower `x == nil` and `x != nil` as a tag test for every optional, including one whose payload
  is not `Equatable`, such as a function or a noncopyable type.
- [ ] Give builtin `Array<T>` conformance when `T` conforms, through a runtime element-compare
  callback, like the clone and destroy callbacks. Check `Dict` separately against the open question
  below.
- [ ] End-to-end fixtures for `Int?`, `Point?`, `Result<Int, String>`, `[Int]`, `[[String]]`, `== nil`
  on a function optional, and noncopyable payloads compared by borrowing.
- [ ] Benchmark scalar, string and array equality in both profiles against the pre-change baseline.

### Phase 5: acceptance

- [ ] Run the adversarial tester, then the type-system and IR/backend reviewers, each separately from
  the author.
- [ ] Run full `scripts/validate.py` in Debug and Release on every supported platform. Report gates
  that were not run.
- [ ] Update `docs/impl/supported-features.md` and the implementation notes. Delete this plan when
  everything is checked.

## Open questions

All are settled in the Phase 1 decision.

- **Requirement shape.**
  - `func equals(other: Self): Bool` with a borrowed receiver and argument, or a different name?
  - Joyeer has no overloading (§12.2, §13, §20.4), so `==` must map to a named requirement.
- **Opt-in or implicit.** Explicit `extension S: Equatable {}` keeps reviewable intent (AI writes,
  humans review), but asks for boilerplate. Implicit structural equality is convenient, but makes
  `==` appear on types whose authors never meant it.
- **Conditional conformance.** A payload-conditional rule for derived conformances only, or the
  general `where` clauses deferred in §20.10 item 1?
- **`nil` comparison.** Should the tag test stay a special form, or would requiring `T: Equatable` for
  `x == nil` be clearer? Function optionals are the main case for the tag test.
- **Floats.** Should `Float` and `Double` conform although IEEE equality is not reflexive, or get a
  separate rule? Today's behavior must not change for scalar `==`.
- **`Dict`.** Order-independent equality needs key lookup per entry. Should `Dict` get equality now,
  or wait for the library `Dict` of plan 0011 Phase 4 and a `Hashable` key decision?
- **Noncopyable and linear types.** Equality only borrows. Can every noncopyable type conform, and
  can a linear type?
- **Cross-module conformance.** Today a conformance must be in the type's module. Is that enough for
  the derived form, given the builtin conformances?

## Out of scope

- `Comparable` and `<` on user types.
- `Display` and string interpolation.
- `Hashable` and broadening `Dict` key types.
- Identity or reference equality.
- Operator declarations or overloading.
- Default protocol implementations beyond the derived `Equatable` witness.
- Existential `any Equatable`.

## Progress log

- 2026-11-14: Created after 0024 rejected `==` on `Optional`/`Result`. All items are open.

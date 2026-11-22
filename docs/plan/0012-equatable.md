# 0012: Equality through `Equatable`

- Status: in progress (updated 2026-11-22); phases 1 to 4 are done, and phase 5 is done on Windows x64; validation on the other platforms remains
- Decisions: [0021](../decisions/0021-protocols-and-generic-functions.md),
  [0022](../decisions/0022-generic-types-and-layout-intrinsics.md),
  [0024](../decisions/0024-library-optional-and-result.md) (rejects `==` on `Optional`/`Result`),
  [0029](../decisions/0029-equatable.md) (this plan's decision, accepted 2026-11-21)
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
  They lower to the conformance's witness, or to the existing scalar `compare` and the inlined `String`
  prelude call, with no extra call for builtin scalars.
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
- **Lowering:** equality takes one of two paths
  ([0028](../decisions/0028-ir-opcode-set.md) stage 3):
  - Scalars lower to the `compare` opcode, which accepts only scalar operands. `Builder::comparison`
    (`native_functions.cpp`) emits `FCMP_OEQ`/`UNE` for floating point and `ICMP_EQ`/`NE` for integers
    and `Bool`.
  - `String` `==` calls the collections prelude's `string_equal` role function
    (`lib/Runtime/collections.joyeer`), which is always inlined and calls `joyeer_string_equal_abi`;
    `!=` is `compare eq` of that result with `false` (`emitStringOperator` in `irlowering.cpp`).
    Only literal String patterns in `switchPattern` still call the runtime from the backend
    (`Builder::stringsEqual`).

  Before 0024 and the 0026 array switch, `[Int] == [Int]`, `[Int: Int] == …` and optionals were
  accepted but did not lower, and no fixture compared them.
- **After 0024:** `Optional` and `Result` reject `==`, with help pointing to `match`.
- **After the 0026 array switch:** arrays are ordinary role-bound structs, so the existing struct rule
  rejects `[Int] == [Int]` during type checking rather than accepting it and failing LLVM verification.
  The array switch did not implement array equality.
- **Spec:** `Equatable` is reserved and unusable (§12.4, §20.2, `type-checking.reserved-protocol-name`).
  Generic code cannot use operators on `T` (§20.5).
- **Gaps:** there are no default implementations, conditional conformances or operator declarations
  (§20.10). Conformances of builtin types and conformances outside the type's module are not
  implemented (§3.5).

## Todo

### Phase 1: decision

- [x] Record a decision for `Equatable`. It must settle the open questions below and state how each
  rejected option fails (operator functions, implicit structural equality for every type). Evidence:
  [0029](../decisions/0029-equatable.md), status proposed; the user chose four of the answers (2026-11-21).
- [x] Run the language-designer and type-system reviewers on it; record the outcome in the decision.
  Evidence: 0029 "Review": one critical, three high and the medium and low findings, all addressed in the
  revision.
- [x] The user accepts 0029 as revised (status `accepted`). Evidence: the user's acceptance on
  2026-11-21; 0029's status line.
- [x] Amend the sections 0029 lists (§2.1.2, §2.4, §2.5, §2.6, §5.2, §12.4, §20.2, §20.3, §20.4.2,
  §20.5, §20.6, §20.8, §20.10, §20.12.2, §20.12.5, Appendix C) and 0024. Add §20.13 for `Equatable`
  with its diagnostics. Evidence: §20.13 (marked specified, not implemented), the listed sections and
  0024's amendment notes; a search of `specs/` finds no remaining rule that rejects `==` on `Optional`,
  `Result` or user types, or reserves `Equatable` as unusable. Documentation only; nothing built.

### Phase 2: builtin types and the operator

- [x] Write failing tests:
  - type-checking stage fixtures for every accepted and rejected `==` form of this phase (the forms
    0024 rejects, on `Optional` and `Result`, move to phase 4);
  - GoogleTests for conformance lookup and the scalar fast path.
  Evidence: e70e44b (17 of 20 observed failing first), 8be2a99 (review findings, observed failing
  first); `TypeCheckingTest.Equatable*`, `*Equality*`, `IRLoweringTest.LowersEqualityToTheBuiltin*`,
  `EqualityReachesGenericWitnessInstances*`, `E2EEquatableBuiltins` and `E2EEquatableWitnessInstances`
  at -O0 to -O3, `TypeGenericEqualityUnconstrained`, `TypeEquatableLinear`.
- [x] Make `Equatable` a compiler-known protocol bound by a closed prelude role, like 0024, not by
  its name. State its requirement. Evidence: 952c0e7; `lib/Runtime/roles.joyeer` declares
  `@compiler_role(equatable) public protocol Equatable { func equals(other: Self): Bool }`, bound by
  `SemanticModel::roleSymbol(LibraryRole::equatable)`.
- [x] Give builtin conformances to the integer types, `Bool`, `UInt8`, `Float`, `Double` and `String`.
  Keep their current lowering, so code size and speed do not change: the scalars to the `compare`
  opcode, and `String` to the prelude's `string_equal` role call. Decide whether `String`'s witness is
  that role function or a new one that wraps it. Evidence: `TypeCheckedModel::isBuiltinEquatable`
  (also `Void`); builtins have no witness method, so `String` keeps the `string_equal` call and
  `t.equals(other:)` on a builtin instance lowers to the builtin `==`
  (`LowersEqualityToTheBuiltinOperationOrTheWitness`: `generic<Int>` has one `compare` and no call).
  Lowering is unchanged for non-generic builtin code; the speed gate is phase 4's benchmark.
- [x] Replace the kind test in `binaryResult` with conformance lookup. Keep `invalid-operator-operands`
  and add help naming `Equatable`. Evidence: 952c0e7, ed38353 (help names only what the compiler
  accepts); `EqualityHelpNeverSuggestsAConformanceTheCompilerRejects`.
- [x] Accept `==` on `T` when the constraint lists `Equatable` (§20.5). Lower it to the witness after
  monomorphization. Evidence: 952c0e7, ed38353 (the witness instance is required like a call of
  `equals`); `generic<Money>`, `same<Box<String>>` and `Box<T>.matches` call their witnesses.
- [x] Hand-written conformances of user types, and `==` on them borrowing both operands (§20.13.2),
  brought forward from phase 3 because generic `==` needs them. Evidence:
  `AHandWrittenEquatableConformanceGivesEqualityAndSatisfiesTheConstraint`,
  `ANoncopyableConformerComparesByBorrowing`, `NonBuiltinEqualityHoldsItsLeftOperandAcrossTheRight`,
  `NonBuiltinEqualityRejectsAMoveOfTheLeftOperandInsideTheRight`.
- [x] Type-system and IR/backend reviews of this phase. Evidence: both found the same high issue (a
  generic type's witness instance never required by `==`) and medium and low ones; all fixed in
  ed38353 with tests in 8be2a99, except the two items added below.
- [x] Debug and Release CTest. Evidence: at ed38353 on Windows x64, Debug 2074 of 2074 and Release 2073
  of 2073. `scripts/validate.py` (with `fern test`) waits for phase 5.
- [x] Check the shape of the role prelude's `Equatable` in `validatePreludeContracts` (one borrowing
  `equals(other: Self): Bool`, one declaration with the role), as for the other roles. Found by the
  type-system review; today a malformed bundle is found only by a failed lookup. Evidence: aee8810 tests
  (observed failing), 55ae981; `EquatableRoleTest.TheEquatableRoleShapeIsChecked`, and for the
  `array_equal` function `CollectionRoleTest.TheArrayEqualRoleShapeIsChecked`.

### Phase 3: derived conformances

- [x] Type an operand without a type of its own (`nil`, `.Red`, `[]`) against the other operand on
  either side, and stop passing the enclosing expected type (§20.13.2), so `.Red == c` works with a
  derived conformance. Evidence: 23d9945, 5242c84 (literal arrays, open generic cases);
  `AnOperandWithoutATypeOfItsOwnIsTypedByTheOtherOnEitherSide`,
  `LiteralAndCaseOperandsAreTypedByTheOtherSide`; `nil == a` is now typed and waits for phase 4.
- [x] Accept `extension S: Equatable {}` with no body on a user struct or enum, and derive memberwise
  (or case-then-payload) equality. Reject it when a field or payload is not `Equatable`, naming the
  field. Evidence: c42dd76 tests (12 observed failing), 23d9945; `AnEmptyConformanceDerives*`,
  `ADerivedConformanceRejectsAMemberThatCanNeverConformAtTheExtension`, `TypeEquatableDerivedMember`,
  `DerivedEqualityIsAGeneratedFunctionPerConcreteType`.
- [x] Accept a hand-written `equals` witness in place of the derived one. Evidence: any member named
  `equals` selects the hand-written path, also from a later extension
  (`AMemberNamedEqualsSelectsTheHandWrittenPath`, `AnEqualsMethodInALaterExtension*`,
  `DerivedWitnessesFollowWhatTheProgramReaches`).
- [x] For a generic type, define when `Box<X>` conforms: the derived conformance holds when every
  field type, with `X` substituted, conforms. Check it at use, and record the rule in the decision.
  Evidence: 0029 and §20.13.5 record it; `ADerivedConformanceOfAGenericTypeIsConditionalOnItsArguments`,
  `DerivedConformanceBoundariesAtTheExtensionAndAtTypeArguments`.
- [x] End-to-end fixtures: compare structs, payload enums and generic instances; reject the
  non-conforming ones. Evidence: `E2EEquatableDerived` and `E2EEquatableDerivedEdges` at -O0 to -O3,
  and the -gdwarf/-gcodeview compiles of `equatable_derived`.
- [x] Recursive types: compare a recursive enum or struct. `indirect` cases are not implemented, so
  recursion goes through arrays (phase 4). Evidence: `RecursiveTypesConformThroughArrays` (trees, JSON
  enums, mutual recursion) and the `Tree` and `Json` cases of `E2EEquatableContainers`.
- [x] Type-system and IR/backend reviews of this phase. Evidence: their findings (a stored-field
  default without its witness, a `-g` LLVM verification failure, `Never` operands, cascading
  diagnostics, exponential checks, operand typing, witnesses for uninstantiated bodies, help text)
  have tests in 9c3c654 (9 observed failing) and fixes in 5242c84.
- [x] Debug and Release CTest of phase 3. Evidence: at 5242c84 on Windows x64, Debug 2104 of 2104 and
  Release 2102 of 2103; `ValidationScriptTests` timed out under parallel load and passed alone.

### Phase 4: Optional, Result and containers

- [x] Give the `optional` and `result` role enums the derived, payload-conditional conformance.
  Remove 0024's rejection and its `match` help. Evidence: 51b9953 tests (11 observed failing), 872a1b2;
  the role prelude's two empty `Equatable` extensions, the only extension the role enums admit;
  `OptionalResultAndArraysConformWhenTheirComponentsDo`, `TypeOptionalEqualityRejected` and
  `TypeResultEqualityRejected` now with non-conforming payloads; help names the payload (f984011).
- [x] Lower `x == nil` and `x != nil` as a tag test for every optional, including one whose payload
  is not `Equatable`, such as a function or a noncopyable type. Evidence: `NilComparisonTestsPresence*`,
  `LowersContainerEqualityAndPresenceTests` (no call, one `switchPattern`), `APresenceTestOnALinearPlaceIsAccepted`,
  `APresenceTestOnALinearTemporaryDropsIt`; `.None` and `nil` on either side.
- [x] Give the role-bound library `Array<T>` conformance when `T` conforms, without reviving a compiler array
  kind or dispatching on the name `Array`. Decide library witness iteration versus a runtime element-compare
  callback in the equality decision. Check `Dict` separately against the open question below. Evidence:
  library witness iteration, as 0029 decided: `@compiler_role(array_equal) arrayEqual<T: Equatable>` in
  the collections prelude; `Dict` does not conform; `ArrayCharacterization.EqualityOfLibraryArrays*`.
- [x] End-to-end fixtures for `Int?`, `Point?`, `Result<Int, String>`, `[Int]`, `[[String]]`, `== nil`
  on a function optional, and noncopyable payloads compared by borrowing. Evidence:
  `E2EEquatableContainers` and `E2EEquatableOwnership` (each element and temporary destroyed once) at
  -O0 to -O3, and the -gdwarf/-gcodeview compiles of the containers fixture.
- [x] Type-system and IR/backend reviews of this phase. Evidence: the IR review found nothing critical or
  high; the type-system review's two high findings (presence test on a linear place, a consumed member
  call) and its medium and low ones have tests in bf42aa7 and fixes in f984011; the consumed member call
  was already rejected by type checking (0e45997). Not fixed here: the IR verifier accepts `switchPattern`
  cases after a wildcard and non-exhaustive switches (older than this plan).
- [x] Debug and Release CTest of phase 4. Evidence: at f984011 on Windows x64, Debug 2128 of 2128 and
  Release 2127 of 2127.
- [x] Benchmark scalar, string and array equality in both profiles against the pre-change baseline.
  Evidence: `tests/perf/benchmark_equality.py`, Windows x64, baseline compiler cf4fc78 (before phase 2)
  against be17da5, both Release builds, 30 interleaved repetitions, paired median ratio and 95% bootstrap
  interval. Arrays and derived structs have no baseline `==`, so the baseline compiles a hand-written
  loop and field comparison. -O0: scalars 1.019 [1.009, 1.029], rerun at 60 repetitions 1.005 [0.998,
  1.013] (the two compilers emit identical IR for it); strings 1.005 [0.994, 1.010]; arrays 0.993
  [0.983, 1.000]; derived 0.677 [0.669, 0.681]. -O2: scalars 1.000 [0.981, 1.033]; strings 1.008
  [0.997, 1.017]; arrays 0.988 [0.963, 1.053]; derived 1.005 [0.927, 1.033]. Executable sizes are
  equal in every row. Raw reports in `out/perf-gate/equality`.

### Phase 5: acceptance

- [x] Run the adversarial tester, then the type-system and IR/backend reviewers, each separately from
  the author. Evidence: the adversarial run at 819d220 (no -O0/-O2 differences or ownership bugs; a
  requirement call on an array instance failed in lowering, operand typing depended on the side, help
  could suggest a duplicate method) has tests in aee8810 and fixes in 55ae981. The final reviews at
  55ae981: IR/backend nothing above low (recorded in ir.md, backend.md and spec 20.13.4); type system
  two medium (cascading reports, help suggesting impossible extensions) and lows, with tests in
  bb65dbe and d41d071 and fixes in 1d9d006. The review's claimed `(1 as Float) == 0.5` regression is
  rejected by the pre-`Equatable` compiler too (`as` is integer-only).
- [ ] Run full `scripts/validate.py` in Debug and Release on every supported platform. Report gates
  that were not run. Windows x64 done: at 1d9d006, Debug and Release each build, pass CTest 2140 of 2140
  (Fern.Workspace, and in Release Fern.Manifest, passed on the validator's rerun) and `fern test` 7 of 7.
  Not run: Linux x86_64, macOS and ARM64 (no host here).
- [ ] Update `docs/impl/supported-features.md` and the implementation notes. Delete this plan when
  everything is checked.

Follow-ups outside this plan, found during it: the IR verifier accepts `switchPattern` cases after a
wildcard and non-exhaustive switches; derived witnesses take aggregates by value, which is slow to compile
for deeply nested structs (ir.md, copy-elision item); the `overlapping-access` message says "in the same
call" for `==`.

## Open questions

All are answered in [0029](../decisions/0029-equatable.md): the requirement is
`equals(other: Self)`; conformance is opt-in and derivable; conditional conformance is structural and
limited to derived `Equatable`; `x == nil` is a presence test; `Float` and `Double` conform with IEEE
equality; `Dict` does not conform until a `Hashable` decision; noncopyable types may conform and linear
types may not; conformances stay in the type's module.

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
- **`Dict`.** Order-independent equality needs key lookup per entry. Should the
  [library `Dict`](../decisions/0027-library-dict.md) get equality now or wait for a `Hashable` key decision?
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

- 2026-11-22: Phase 5 on Windows x64: the adversarial tester and the final type-system and IR/backend
  reviews, their findings fixed (aee8810/55ae981, bb65dbe/d41d071/1d9d006), phase 2's shape check done,
  and `scripts/validate.py` passing in Debug and Release at 1d9d006. The other platforms are not run.
- 2026-11-22: Phase 4's benchmark ran (`benchmark_equality.py`): no regression outside noise; the one
  row whose first interval was above 1 (scalars at -O0) has identical IR and was within noise when
  rerun. Phase 4 is done.

- 2026-11-22: Phase 4 implemented (51b9953 tests, 872a1b2, bf42aa7 and f984011 review findings).
  Recursion through arrays closes phase 3's open item. A replacement role prelude must now declare
  `Equatable`, which the collections prelude uses. The benchmark stays open.

- 2026-11-21: Phase 3 implemented (c42dd76 tests, 4616d7c, 23d9945 implementation, 9c3c654 and
  5242c84 review findings). Recursive types wait for phase 4 arrays (no `indirect`). The full Debug run
  of 23d9945 passed 2087 of 2088 (`ValidationScriptTests` timed out under load).

- 2026-11-21: Phase 2 done (e70e44b tests, 952c0e7 implementation, 8be2a99 and ed38353 review findings).
  Hand-written conformances came forward from phase 3; the reviews added the prelude-shape check
  (phase 2) and symmetric operand typing (phase 3). Debug and Release CTest pass.

- 2026-11-21: The user accepted 0029 as revised. Amended chapters 2, 5, 12 and 20 (new §20.13, marked
  not implemented), Appendix C (`prelude_protocol`) and 0024. Phase 1 is done; phase 2 starts with
  failing tests.
- 2026-11-21: Phase 1 started. Drafted 0029 with the user's four choices (opt-in, `equals(other:)`,
  `== nil` presence test, noncopyable but not linear); ran the language-designer and type-system
  reviewers and revised for every finding (derived witnesses are not members, conditional conformance
  is a recorded predicate, `==` on non-builtin conformers borrows like a call). Added the acceptance
  item; the spec waits for it.
- 2026-11-21: Current state and the phase 2 builtin-conformance item updated for 0028 stage 3:
  `String` equality is now an inlined prelude call, and `compare` takes only scalars. No item changed.
- 2026-11-14: Created after 0024 rejected `==` on `Optional`/`Result`. All items are open.

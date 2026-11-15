# 0011: Library enums and collections

- Status: in progress (updated 2026-11-14)
- Decisions: [0021](../decisions/0021-protocols-and-generic-functions.md),
  [0022](../decisions/0022-generic-types-and-layout-intrinsics.md),
  [0023](../decisions/0023-library-concurrency.md) (completed concurrency-library migration),
  [0024](../decisions/0024-library-optional-and-result.md) (Phase 1, accepted and merged in #26; macOS validation open),
  [0025](../decisions/0025-library-collection-capabilities.md) (Phase 2 capabilities, proposed)
- Spec: [§2](../../specs/02-types.md), [§20](../../specs/20-protocols.md), and the affected
  ownership, expression, and pattern chapters in the [spec index](../../specs/README.md)

## Goal

Replace the remaining compiler-defined generic containers with ordinary Joyeer library declarations:
`Optional` and `Result` first, then `Array` and `Dict`. Preserve their syntax, public APIs, ownership,
diagnostics, and performance while removing container-specific type kinds and executable lowering.

## Definition of done

- `Optional<T>` and `Result<T, E>` are source-library enums; optional sugar, contextual cases,
  exhaustive matching, propagation, and native wrapper signatures remain compatible.
- `Array<T>` and `Dict<K, V>` own their buffers and implement their existing operations in Joyeer.
  An array of linear values can be drained and ended without enabling implicit destruction of them.
- Borrowing an element does not move or clone it, and mutation cannot invalidate a live borrow.
- Removed compiler machinery is replaced by generic capabilities, not dispatch on public names.
- Interleaved benchmarks show no regression outside measured noise in both profiles.
- Separate role reviews and full Debug/Release platform validation are recorded with evidence.

## Current state

[Plan 0010](0010-generics.md) provides generic types, monomorphization, and runtime-only layout
intrinsics. Its concurrency migration is implemented and validated on Windows x64 and Linux
x86_64; 0023 supplies a bundled source prelude, typed stack slots, specialized callback addresses,
and closed task transfer parameters. Native macOS/AArch64 acceptance remains unrun.

`Optional`, `Result`, `Array`, and `Dict` still have compiler type kinds and lowering rules.
Array/dictionary algorithms already live in `lib/Runtime/core/`; the compiler still supplies
layouts, ownership callbacks, construction, and element access. There is no borrowed raw-slot
operation. General user generics still reject linear type arguments, and a raw-address owner
cannot express element-dependent copyability, linearity, and destruction through its fields alone.

## Todo

### Phase 1: Optional and Result

- [x] Record a decision for canonical library enum identities, optional sugar, and propagation.
  Do not merely replace builtin-name dispatch with enum-name dispatch. Evidence:
  [0024](../decisions/0024-library-optional-and-result.md), accepted by the user 2026-11-14 after
  three language-designer reviews.
- [x] Update the normative rules and add failing stage tests and durable fixtures for every
  existing case, pattern, optional wrapping, propagation, and ownership boundary. Evidence: spec
  (§2, §4.1.1, §5.2, §5.4.1, §8.3, §19.4.7, §20.12, Appendix C); equality tests (written first, see the
  2026-11-14 log); `RolePreludeTest` (shape, duplicate role, prelude extension, missing role),
  `IRModelTest.VerifierRejectsMalformedRoleEnumerations`, `CInteropTest.RoleBindingsSurviveLoweringAndTheWire`
  (lowering and wire), `SemanticAnalysisTest.WildcardsOverAJoinHandleInsideOptionalAndResultDropALinearPayload`,
  `E2EPropagation` (`Never` forms) and `E2EOptionalNestedLayers`.
- [x] Declare the enums in the source prelude and preserve source-module visibility/shadowing. Evidence:
  `lib/Runtime/roles.joyeer` in a separate role prelude (a second source ID, always loaded); Windows x64
  Debug `ctest` 1703/1703 on 2026-11-15, including `UserOptionalDoesNotCaptureTheOptionalSugar`.
- [x] Implement generic enum construction, payload access, matching, and propagation through
  concrete typed/IR models; preserve diagnostics and source/debug locations. Evidence: the same
  Debug run; `EnumerationDefinition` carries the role and case indices, wire version 19, and the
  backend reads present/absent tags from it. Reviews and other platforms are open (see below).
- [x] Remove the dedicated type kinds and cases only after compatible fixtures pass. Evidence: `TypeKind::optional`
  and `result`, their builtin symbols and the case-spelling comparisons are gone; the same Debug run
  passed every existing fixture. `==`/`!=` rejection is the one intended change (the new equality tests).
- [x] Measure representative compiler output size and runtime behavior in both profiles. Evidence: the
  2026-11-15 measurement entry below (executables byte-for-byte the same size, LLVM IR identical after
  type-id normalization, runtime within noise).
- [x] Run the role reviewers (type-system and IR/backend, twice each, separately from the author) and
  validate on Windows x64 and Linux x64 (WSL Ubuntu). Evidence: Windows `scripts/validate.py` Debug and
  Release `ctest` 1717/1717 and `fern test` 7/7 (three load-sensitive tests passed on the validator's
  rerun); Linux Debug and Release `ctest` 1693/1693 and `fern test` 7/7; both on commit c0e39c3.
- [ ] macOS validation: run `python3 scripts/validate.py --llvm-root <sdk>` on macOS (Debug and Release,
  unfiltered CTest and fern test). Owner: the user. Not run from the Windows/WSL environment.
- [x] Phase 1 follow-ups found while implementing 0024 and by its reviews: the role-prelude test seam
  and the malformed-shape, duplicate-role, missing-role and prelude-`extension` tests; per-rule shape
  messages; constraint and linear-marker rejection; the lowering and wire tests; the driver's source-table
  check; the unsatisfied-constraint help; the missing-role span; `docs/impl/`. The concurrency prelude's
  `Result<Void, ChannelClosed>` line in `--report-instantiations` is pinned by the regenerated
  `tests/type-checking/generics_report.txt` and `generic_types_report.txt`.

### Phase 2: borrowing and linear collection ownership

- [ ] Record a separate decision for borrowed slot access, projection lifetime/exclusivity,
  and element-dependent ownership of a raw-address container. Drafted as
  [0025](../decisions/0025-library-collection-capabilities.md) (proposed); open until a
  language-designer review accepts it.
- [ ] Decide how generic code transfers or ends linear elements without allowing them to be
  silently dropped. The closed task-transfer exception in 0023 is not a general solution.
  Proposed in 0025 section 4 (must-transfer bodies plus an ending method); open with the item above.
- [ ] Add failing ownership tests for mutation during a borrow, escaped borrows, partial moves,
  linear elements, early exits, and expanding generic recursion.
- [ ] Implement the minimum runtime-only capability and verification needed by safe library
  collection wrappers; reject unauthorized and malformed uses.
- [ ] Review the type/ownership and IR/ABI changes separately from their author.

### Phase 3: Array

- [ ] Benchmark construction, append/growth, borrowed indexing, removal, nested destruction,
  and draining arrays of linear handles before changing the implementation.
- [ ] Implement the existing array API in Joyeer, including constant-array clone boundaries,
  exact allocation/free sizes, and `consumeEmpty` and its trap.
- [ ] Preserve subscript/storage-path/exclusivity behavior and existing diagnostics/fixtures.
- [ ] Remove array-specific opcodes, type kinds, and backend layout/ownership cases.
- [ ] Benchmark again in both profiles, complete adversarial/role reviews, and run full
  platform validation.

### Phase 4: Dict

- [ ] Record a decision for key constraints and hashing/equality before broadening accepted
  key types or changing the existing linear-key/value prohibition.
- [ ] Benchmark construction, lookup, insertion/replacement, growth, and nested destruction.
- [ ] Implement the existing dictionary API and buffer ownership in Joyeer, preserving the
  documented copy-on-lookup and value-semantics boundaries.
- [ ] Remove dictionary-specific opcodes, type kinds, and backend cases after fixtures pass.
- [ ] Benchmark both profiles, run all role reviewers and full platform validation, and
  update supported features and runtime/IR notes.

## Open questions

- How are syntax sugar and `?` tied to a canonical library declaration without reopening
  arbitrary user enum layouts? Settle in the Phase 1 decision.
- What is the type/IR representation of a borrowed raw-slot projection, and what pins its owner
  while it is used? Settle before Array implementation.
- How can raw-address containers express conditional ownership properties and safely hold
  linear elements? A `deinit` on every instance or an always-linear array would change behavior.
- Should dictionary lookup continue cloning noncopyable values only by rejecting them, or gain a
  separately named borrowing API? Keep current behavior until a distinct decision accepts a change.

## Out of scope

Closures, existential/dynamic protocols, a GC, shared mutable containers, new collection APIs,
multi-sender channels, general explicit user type arguments, and performance-driven allocator
redesign. No prerequisite is considered accepted merely because this plan names it.

## Progress log

- 2026-11-14: Created the Phase 4 follow-up plan requested by 0010. All implementation and
  acceptance items in this plan remain open; 0023 records the completed x86_64 concurrency prerequisite.
- 2026-11-14: Drafted [0024](../decisions/0024-library-optional-and-result.md) (proposed) for
  Phase 1: role-bound prelude enums with shape-identified cases. The decision item stays open until
  a language-designer review accepts it.
- 2026-11-14: The first language-designer review of 0024 asked for revision. It found two blocking
  gaps: linear payloads checked against unsubstituted case types, and `Never` arguments used by
  `tests/e2e/propagation.joyeer`. It also found silent behavior changes and misstated diagnostic
  stages. 0024 is revised to address them and awaits a second review.
- 2026-11-14: The second review of 0024 accepted it with changes, and the changes are applied.
  `==`/`!=` on `Optional`/`Result` are now rejected; restoring them through `Equatable` is
  [plan 0012](0012-equatable.md). The decision item stays open: the applied changes have not been
  re-reviewed.
- 2026-11-14: Phase 1 inventory, as 0024 requires before implementation (commit `886c6a9`).
  Line numbers drift; re-run the greps below before each step.
  - `TypeKind::optional`/`result` (48 uses, `grep -rnE "TypeKind::(optional|result)\b" compiler`):
    `frontend/typechecking.cpp` 31 (66, 67, 302, 309, 368, 372, 534, 535, 636, 637, 1972, 3545, 3591,
    3615, 3618, 3629, 3630, 3646, 3650, 4009, 4010, 4034, 4133, 5818–5833, 6042, 6341, 6342);
    `ir/irlowering.cpp` 7 (552, 553, 689–693, 2313, 3703); `ir/ir.cpp` 5 (208, 209, 441, 2131, 2271);
    `backend/native_types.cpp` 4 (162, 163, 218, 219); `frontend/semanticanalysis.cpp` 1 (743).
  - Case-spelling dispatch (`grep -rnE '"(Some|None|Ok|Err)"' compiler`): synthesized case symbols
    `frontend/nameresolution.cpp` 825–843; `frontend/typechecking.cpp` 5818–5824;
    `frontend/semanticanalysis.cpp` 1210–1212; `ir/irlowering.cpp` 689–693, 1452, 1663–1664,
    2315–2319, 2621, 3706 (`findEnumCase` by name); `backend/native_collections.cpp` 162–163,
    228–229 (`removeLast`/`get` tags).
  - Type-name lookup: `frontend/nameresolution.cpp` 743–744 (`declareBuiltinType`), 1847
    (`T?` binds `"Optional"`); `frontend/typechecking.cpp` 66–67 (`registerGenericBuiltin`), 133, 152
    (`builtinSymbol`); `frontend/platform.cpp` 67 (syntactic `Optional<X>` printed as `X?`; must not
    sugar a user `Optional`).
- 2026-11-14: Third review of 0024 gave "accept with changes"; the changes are applied (role-prelude
  load order, verifier wording for per-instance definitions, no-cascade mechanism, diagnostic stages,
  equality test forms, display in the unnamed root module, spec list, removal list). Chosen with the
  user: the enums live in a separate always-loaded role prelude, so test-supplied preludes keep
  working. The decision item stays open until a final language-designer pass confirms these edits.
  Nothing is implemented; no build or test was run (documentation only).
- 2026-11-14: Final review of 0024 gave "accept with changes": four blockers (source IDs and source
  tables for two prelude sources, the removal list wrongly dropping IR lowering's shared builtin
  enumeration collection, duplicate-role scope breaking the 0023 roles test, and a test-only seam
  for malformed role preludes) plus wording fixes. All are applied to 0024. One choice was made
  without asking: a test-only `ModuleInput` flag replaces the bundled role prelude. The decision
  item stays open until the user accepts the record. Documentation only; nothing built or run.
- 2026-11-14: The user accepted 0024. Checked the decision item and amended the spec for it: §2.4–2.6
  and §2.9, §4.1.1, §5.2, §5.4.1, §8.3, §19.4.7, §20.12.3/.5, Appendix C (`prelude_enum`), plus a
  pointer in 0023. The spec item stays open until the failing stage tests and durable fixtures exist.
  `docs/impl/` notes (supported-features, ir, backend, runtime) change with the implementation, not
  now. Documentation only; nothing built or run.
- 2026-11-14: Tests first, part 1 (Windows x64 Debug, `out/build/x64-debug-comp`). Added
  `TypeCheckingTest.EqualityOnOptionalAndResultIsRejectedWithMatchHelp` and the stage fixtures
  `tests/type-checking/err/{optional,result}_equality.joyeer` (`TypeOptionalEqualityRejected`,
  `TypeResultEqualityRejected`). Observed failing for the expected reason: the checker accepts `==`
  on these types today, so there is no `invalid-operator-operands` (the CLI fixtures then fail with
  that diagnostic missing). Also added passing guards that pin behavior 0024 must keep:
  `NilOnTheLeftOfEquality…`, `UserOptionalDoesNotCaptureTheOptionalSugar`,
  `NestedOptionalKeepsItsOuterLayerForNone`, `OptionalFunctionValuesStayRejected…`,
  `UserGenericsStillRejectLinearOptionalArguments`. Running them found two errors in 0024, now
  corrected there: `Int??` lexes as the `??` operator, and `Optional.Some(nil)` is rejected today
  (`missing-contextual-type`), so it was dropped from the nesting test. The full
  `CompilerTypeCheckingTests` binary has one failure, the new equality test; the rest pass. Not run:
  the rest of the suite, Release, Linux. The item above stays open.
- 2026-11-15: Phase 1 implemented (Windows x64 Debug only). The role prelude `lib/Runtime/roles.joyeer`
  declares `Optional` and `Result`; name resolution binds `T?` and the builtin `get`/`removeLast` to the
  role symbol; `TypeKind::optional`/`result` and every case-spelling comparison are removed in favor of
  `TypeContext::isOptional`/`isResult` and the IR's `EnumerationRole` (wire version 19). The full
  Debug `ctest` passes 1703/1703, including the equality tests that failed first. Two corrections to
  0024: the role attribute is spelled `optional_type`/`result_type` because `result` is a reserved
  word, and `Result<Void, ChannelClosed>` now appears in `--report-instantiations` for programs that do
  not use channels (the expected files are regenerated with the compiler). Not done: the type-system and
  IR/backend reviews, Release, Linux, macOS, benchmarks, `docs/impl/`; none of this is committed.
- 2026-11-15: Reviews of Phase 1 (type-system and IR/backend, separate agents). Neither found a
  soundness or destruction hole; the IR reviewer saw identical O0/O2 output and unchanged layout and
  tags. Fixed: (1) `Optional.None` / `Optional.Some(x)` against an expected `Optional<Int?>` looked
  through a layer and built the inner one (`Optional.None` printed the inner layer; 0024's "as today"
  was wrong, the old binary rejected these expressions). Both look-through sites now skip the optional
  role's own declaration; new fixture `tests/e2e/optional_nested_layers.joyeer` (`E2EOptionalNestedLayers`
  O0-O3) printed `1 0 0 1` before the fix against the expected `0 0 0 1`, and passes now. (2) `Result`
  success/failure were bound by position in lowering but accepted in either order by the checker; the
  shape check now requires `Ok(T), Err(E)` order, and 0024 says so. (3) The verifier now rejects a role
  value outside the closed set, one declaration bound to both roles, payloads that disagree with the type
  arguments, a swapped result pair and a non-enumeration role type (`IRModelTest.VerifierRejectsMalformedRoleEnumerations`;
  written after the verifier change, so not observed failing). (4) A user `Optional`/`Result` is named
  `<main>.Result` in every diagnostic, and `nil`/`?` on it carries a help line
  (`UserResultShadowingExplainsItselfAndDisplaysQualified`). (5) The IR dump prints the role.
  Decided: the syntactic `Optional<X>` to `X?` spelling in `platform.cpp` stays, see 0024. Stale wire
  version 18 and builtin-identity text updated in `docs/impl/` (`ir`, `backend`, `c-interop`,
  `name-resolution`, `runtime`). Windows x64 Debug `ctest` passes 1710/1710. Open: the follow-up item
  above, `supported-features.md`, Release/Linux/macOS, benchmarks; none of this is committed.
- 2026-11-15: Phase 1 follow-ups. `ModuleInput::rolePreludeContext` (test-only, never set by the driver)
  replaces the bundled role prelude, so `RolePreludeTest` covers a well-shaped replacement, a swapped
  `Result`, three cases, a labelled payload, a constrained or extra type parameter, a wrong payload, a
  duplicate role, a prelude `extension` of a role enum, and a missing or non-enum role. The validator now
  says which shape rule failed and rejects constraints on the role parameters; a prelude `extension` of
  either enum is `invalid-prelude`. An empty payload clause (`None()`) is already a parse error, so no
  check was added. `CInteropTest.RoleBindingsSurviveLoweringAndTheWire` asserts the role and case
  indices after lowering (a user `enum Optional` keeps none) and that they survive the wire. The
  driver asserts each prelude source's ID equals its table index; the `extension Int?: P` help no longer
  suggests an impossible fix; a missing role is reported at the role prelude's source. These tests were
  written together with the changes they cover, so none was observed failing first. Windows x64 Debug
  `ctest` passes 1715/1715. Not done: Release/Linux/macOS results, benchmarks, the instantiation-report
  test above; none of this is committed.
- 2026-11-15: Windows x64 Release `ctest` (`out/build/x64-release-comp`, `-j6`): 1714 of 1715 passed. The
  one failure, `ValidationScriptTests`, timed out at 33 s under parallel load and passes alone in 0.5 s,
  so it is load flakiness in a script test, not this change. Linux, macOS, the benchmarks and
  `python scripts/validate.py --llvm-root <sdk>` have not been run; none of this is committed.
- 2026-11-15: Second review pass and fixes. Neither reviewer found a soundness or destruction hole.
  IR/backend: the verifier now requires a result's cases at indices 0 and 1 (equal type arguments made
  a swap undetectable), that the type and its enumeration definition name the same declaration, and an
  honest message for an unknown role value; `VerifierRejectsMalformedRoleEnumerations` now requires each
  rejection to name the role and covers a `Result<Int, Int>` swap. The driver's debug-only assert became
  an unconditional `driver.invalid-source-table` check. Type system: `isAssignable` allowed `Int` into
  `Optional<Int?>` through two layers, which lowering cannot represent (`ir-lowering.missing-type`); one
  promotion now wraps one layer, and a nested optional prints as `Optional<Int?>` since `Int??` cannot
  be written. Prelude errors from declarations (an extension, an annotation) now stop body checking as the
  shape errors do; the test seam implies prelude context; the shadowing help is per role and says how to
  fix it; the equality help shows the `x == nil` replacement; the constraint help shows a `match`.
  Tests that passed vacuously were replaced: `OptionalFunctionValuesStayRejectedWhereFunctionTypesAre`
  now pins `invalid-function-type` for `[((): Int)?: Int]`, and `E2EOptionalNestedLayers` inspects the
  inner layer for `Optional.Some(5)`, `Optional.Some(nil)` and three-level nesting. `scripts/validate.py` on
  commit c0e39c3 passes: Windows x64 Debug and Release `ctest` 1717/1717 and `fern test` 7/7 (`Fern.Manifest`,
  `Fern.Workspace` and `Compression.Interoperability` passed only on the validator's rerun in both
  configurations, a parallel-load timeout pattern also seen earlier); Linux x86_64 (WSL Ubuntu) Debug and
  Release `ctest` 1693/1693 and `fern test` 7/7. (An earlier run on commit 348e33d passed too.)
- 2026-11-15: Output size and runtime measurement (Windows x64, Release compilers, no other work running).
  Baseline: `main` at 886c6a9 built in a separate worktree; candidate: c0e39c3. Programs: `hello`,
  `array_remove_last`, `dictionary_get`, `propagation`, `alloc_heavy`, `print_heavy`, `task_churn` and
  `channel_throughput`, each at -O0 and -O2. Executable sizes are identical in all 16 pairs (for example
  hello 48128 and 43520 bytes, propagation 73216 and 54272, dictionary_get 115712 and 98304). The
  `--emit-llvm` text differs only in type-id numbers (the new compiler interns more types, so an id such as
  `joyeer_clone_type_49` becomes `_54`); after normalizing those numbers all 16 pairs are the same lines.
  Runtime, 15 interleaved rounds alternating A/B order, median new/base: 0.92 to 1.03 for every program
  and profile (task_churn 1.00 to 1.02, channel_throughput 0.99, alloc_heavy 0.99 and 1.03, print_heavy
  1.01 and 0.995, propagation 0.92 and 1.00), with identical output and exit status in each run. A 3-round
  first pass showed channel_throughput at -O0 at 1.17; it did not reproduce in 15 rounds (0.986), so it was
  noise. Compile time is unchanged (for example hello 0.17 s both). The gate in tests/perf/README.md
  (no regression outside noise, bounded hello growth) is met: hello grew by 0 bytes. The paired bootstrap
  interval of `benchmark_concurrency.py` was not run; the ratios above are plain medians.
  Phase 1 remains open only for the macOS validation item.
- 2026-11-15: Phase 2 started on branch `docs/decision-0025-collection-capabilities`. Inventory of the
  current array/dictionary machinery (read-only): the compiler owns the type kinds, the 24-byte layout,
  element access (an inline bounds check plus a typed address, with no user subscript or projection),
  construction and constant arrays, per-type clone/destroy callbacks and all ownership rules, in about
  450 lines of verifier, 450 of lowering, 300 of backend collection code and 90 of type checking; the
  algorithms already live in `lib/Runtime/core/arrays.joyeer` and `dictionaries.joyeer`. Eight obstacles
  to a library `struct Array<T>` are listed in 0025 (no borrowed element projection, unconditional
  noncopyability of a `deinit` struct, linear elements held through an address, the hidden `$live` flag
  breaking the C ABI layout, zero-filled moved-out slots, inline `-O0` access cost). Drafted 0025
  (proposed): a `slot<T>(address)` place, an `array_element` addressor role, `@compiler_owns` and
  `@compiler_copy` prelude metadata, an ending-method rule for linear containers and
  `@compiler_inert_zero`. No code changed; the decision item stays open pending review.
- 2026-11-15: First language-designer review of 0025 requested changes (four blockers: the linear
  destroy and ending story contradicted 0022's `destroy_at` rule and 0015's exit check; the copy method
  could not be written without a clone intrinsic; the owner-pin claim was misattributed and the IR escape
  rule undefined; the inert-zero contract lacked idempotence). Revision 2 replaces three free attributes
  and a source-visible slot with one `array_type` role plus `array_element`, `array_copy` and
  `container_end` method roles and a compiler-internal slot, adds `clone_at` and `is_linear`
  intrinsics, makes the destroy helper zero-fill, and expands the tests. No code changed; the decision
  item stays open until a review accepts revision 2.
- 2026-11-15: Second language-designer review of 0025 revision 2: accept with changes (one blocker, the
  constant-folded dead arm, which missed instance discovery, the checker and the header callbacks).
  Revision 3 replaces it with per-type intrinsics (`destroy_range`, `clone_callback`, `destroy_callback`,
  `clone_at`) that lower per concrete type, pins the `container_end` rule and trap texts, makes the IR
  escape rule dominance-based, adds instance discovery of role-lowered uses, access-mode addressors for
  the dictionary, the wire and verifier consequences and a larger test list. Probed with the current
  compiler: `&a.append(a[0])` is rejected with `overlapping-access`; `consume hs[0]` on an array of
  `JoinHandle` is accepted but the array then fails `use-after-consume` or `linear-not-consumed`, so the
  new `consume-of-linear-element` diagnostic changes no accepted program; an array of `JoinHandle` is a
  valid task input today, so a library array must stay sendable (§21). 0025 stays `proposed` until the
  user accepts it.

# 0011: Library enums and collections

- Status: in progress (updated 2026-11-18)
- Decisions: [0021](../decisions/0021-protocols-and-generic-functions.md),
  [0022](../decisions/0022-generic-types-and-layout-intrinsics.md),
  [0023](../decisions/0023-library-concurrency.md) (completed concurrency-library migration),
  [0024](../decisions/0024-library-optional-and-result.md) (Phase 1, accepted and merged in #26; macOS validation open),
  [0025](../decisions/0025-library-collection-capabilities.md) (Phase 2 capabilities, accepted and merged in #27),
  [0026](../decisions/0026-library-array.md) (Phase 3 library `Array`, accepted)
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

`Optional` and `Result` are library enums; the stage 4 candidate also binds `Array` to the source
declaration in `lib/Runtime/collections.joyeer`. `Dict` is still compiler-defined.
Array/dictionary algorithms live in `lib/Runtime/core/`. The closed collection roles provide borrowed
element places, element-dependent ownership and three-word inert-zero storage; general user generics
still reject linear type arguments. Array cleanup, required reviews and final cross-platform/cost acceptance
remain separate gates.

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
- [x] macOS validation: the integrated candidate passes `scripts/validate.py` on macOS/AArch64 with the
  existing LLVM 22.1.8 and macOS 26.5 SDKs: Debug and Release unfiltered CTest 1823/1823 and fern test 7/7.
  This includes Phase 1's compatibility tests; it is not an isolated rerun of the earlier Phase 1 commit.
- [x] Phase 1 follow-ups found while implementing 0024 and by its reviews: the role-prelude test seam
  and the malformed-shape, duplicate-role, missing-role and prelude-`extension` tests; per-rule shape
  messages; constraint and linear-marker rejection; the lowering and wire tests; the driver's source-table
  check; the unsatisfied-constraint help; the missing-role span; `docs/impl/`. The concurrency prelude's
  `Result<Void, ChannelClosed>` line in `--report-instantiations` is pinned by the regenerated
  `tests/type-checking/generics_report.txt` and `generic_types_report.txt`.

### Phase 2: borrowing and linear collection ownership

- [x] Record a separate decision for borrowed slot access, projection lifetime/exclusivity,
  and element-dependent ownership of a raw-address container. Evidence:
  [0025](../decisions/0025-library-collection-capabilities.md), accepted by the user 2026-11-15 after
  two language-designer reviews (revision 3); spec §2.6, §4.1.1, §4.4, §20.12.5 and §21.2.2 amended.
- [x] Decide how generic code transfers or ends linear elements without allowing them to be
  silently dropped. The closed task-transfer exception in 0023 is not a general solution.
  Evidence: 0025 section 5 (the `container_end` method role, must-transfer bodies of the `array_type`
  declaration, `destroy_range` trapping for a linear type, `consume-of-linear-element`).
- [x] Add failing ownership tests for mutation during a borrow, escaped borrows, partial moves,
  linear elements, early exits, and expanding generic recursion. Written and observed failing first
  (`tests/unittests/compiler/collection_roles_test.cpp`, target `CompilerCollectionRoleTests`, 17 tests,
  see the log); later additions (malformed IR, escape rule, backend emission, reviewer findings) were
  written with their fixes. Completed by the stage 4 candidate: the unchanged characterization fixtures
  execute the real library array (zero states, moved-out slots, drain, evaluation/trap/destruction order);
  `array_task_transfer` covers handle arrays through tasks, `array_library_edges` and the role tests cover
  `Never`/`Void` and expanding recursion, and runtime/C interop tests pin C size/offsets and Joyeer layout.
  Evidence: macOS Release unfiltered CTest 1809/1809, including 59 collection-role tests.
- [x] Implement the minimum runtime-only capability and verification needed by safe library
  collection wrappers; reject unauthorized and malformed uses. Evidence: the log entry of 2026-11-15
  (implementation); `CompilerCollectionRoleTests` 33 of 33; Windows x64 Debug `ctest` passes.
- [x] Review the type/ownership and IR/ABI changes separately from their author. First round done (log);
  final type-system, IR/backend and adversarial follow-ups are recorded below. Evidence: macOS Debug/Release
  validation, Windows x64 and Linux x86_64 `scripts/validate.py` on 656ac2e, and the independent IR/backend and
  type-system reviews of that head (log entry of 2026-11-16, "Review and validation of the pushed switch").

### Phase 3: Array

- [x] Benchmark construction, append/growth, borrowed indexing, removal, nested destruction,
  and draining arrays of linear handles before changing the implementation. Evidence: the baseline of the
  2026-11-15 log entry below (`tests/perf/array_*.joyeer`, `tests/perf/benchmark_arrays.py`).
- [x] Record the Phase 3 decision: [0026](../decisions/0026-library-array.md). The array algorithms are already Joyeer
  (in the runtime), so the deliverable is a library `Array<T>` over them and the removal of the compiler's
  array-specific paths, not a rewrite of the algorithms. Evidence: accepted by the user 2026-11-15 ("accept for now")
  after three language-designer reviews, including the `-O0` amendment of 0025's gate.
- [x] 0026 stage 1, characterization: tests on the builtin array that the switch must keep passing. Evidence: merged
  in #28 (eight e2e fixtures, `CompilerArrayCharacterizationTests`), `validate.py` on Windows and Linux (log).
- [x] 0026 stage 2, measurement: the cost of a library array's element access and drop against the builtin, the fixes
  it requires (`alwaysinline` on the addressor, the `slot_address` intrinsic), and the gate numbers. Evidence: the
  log entries of 2026-11-15 and the Windows x64 gated measurements of 2026-11-16, which pass every section 4 limit
  of 0026 (one rerun used for flat drop at -O2).
- [x] 0026 stage 3, infrastructure: collections prelude/seam/source IDs, `place_address`, `stack_block`,
  `array_literal` and internal linkage. Evidence: commit 7a1d748's Windows/Linux validation and the integrated
  candidate's macOS validator, including 65 collection-role tests.
- [x] 0026 stage 4, the switch: the existing array API in Joyeer (constant-array clone boundaries, exact
  allocation/free sizes, `consumeEmpty` and its trap) as the bundled `Array`, with subscript, storage-path and
  exclusivity behavior, diagnostics and the characterization fixtures preserved; the cost gate of 0026 Section 4.
  Green on macOS Debug/Release (1823/1823). Evidence for the review-fix head 71db2ec: Windows x64 `validate.py`
  Debug and Release `ctest` 1854/1854 and `fern test` 7/7 (flaky `Fern.Manifest`, `Fern.Workspace` passed on rerun),
  Linux x86_64 `validate.py` `ctest` 1830/1830 and `fern test` 7/7 in each configuration; the Windows x64 cost gate
  passes (log, 2026-11-16); the user accepted the implementation amendments A1-A9 of 0026 (revision 4) on 2026-11-16.
- [x] 0026 stage 5, cleanup: no `TypeKind::array` or array-specific executable opcode implementation remains.
  Type slot 10 and opcode slots 35-38 are reserved and rejected by the verifier; the wire is 23.
  Evidence: observed-failing removed-slot test, focused reviews and macOS Debug/Release validator 1823/1823.
- [ ] Benchmark again in both profiles, complete adversarial/role reviews, and run full
  platform validation.

### Phase 4: Dict

- [x] Record a decision for key constraints and hashing/equality before broadening accepted
  key types or changing the existing linear-key/value prohibition. Evidence:
  [0027](../decisions/0027-library-dict.md), accepted by the user 2026-11-16 for stages 1-4 after three
  language-designer reviews: the accepted key set stays `Int`, `Bool`, `UInt8` and `String` (checked at the type),
  the linear-key/value prohibition stays, and hashing and `Equatable` keys stay with plan 0012.
- [x] Benchmark construction, lookup, insertion/replacement, growth, and nested destruction. Evidence:
  `tests/perf/benchmark_dicts.py` and the `dict_*` programs; the measurement stage and the switch's cost gate
  (2026-11-17 log entries), with the one missed `-O2` row accepted by the user on the placement evidence.
- [x] Implement the existing dictionary API and buffer ownership in Joyeer, preserving the
  documented copy-on-lookup and value-semantics boundaries. Evidence: the library `Dict<K, V>` in
  `lib/Runtime/collections.joyeer` behind the 0027 roles (stage 6); the differential against the builtin
  (identical outputs and allocation counts at -O0 to -O3); role reviews and fixes; Windows x64 and Linux x86_64
  validation (2026-11-18 log entry).
- [x] Remove dictionary-specific opcodes, type kinds, and backend cases after fixtures pass. Evidence: 0027 stage 7
  (2026-11-18 log entry): no `TypeKind::dictionary`, builtin dictionary opcode or backend case remains; opcode slots
  39-41 and 50 (`subscript_addr`) and type slot 11 are rejected by the verifier (observed-failing tests 439702d,
  e79cd36); the wire is 25; role reviews and Windows x64 and Linux x86_64 validation.
- [ ] Benchmark both profiles, run all role reviewers and full platform validation, and
  update supported features and runtime/IR notes. Done on Windows x64 and Linux x86_64 for the switch and the
  cleanup, and again after merging `main` (2026-11-18 log entries, last at a14b5f2); remaining: macOS validation.

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
- 2026-11-15: The user accepted 0025. Status set to accepted and the spec amended: §20.12.5 (the
  `array_type` role with its capability table, the three method roles and the four intrinsics), §2.6
  (a pointer, since `Array` stays builtin until Phase 3), §4.1.1 (the role is copyable exactly when its
  owned type is, although it has `deinit`), §4.4 (addressor, ordering, `consume a[i]`) and §21.2.2 (a
  library array stays a valid task input exactly when its element is). Tests first, part 1:
  `tests/unittests/compiler/collection_roles_test.cpp` (new target `CompilerCollectionRoleTests`, label
  `collection-roles`) builds a small `Buffer<T>` bound by the role through the 0024 role-prelude seam and
  covers shape validation (each malformed type and method form, a duplicate, an `extension`), use outside
  the prelude, element typing, read-only `count`, exclusivity, copyability and linearity, ending a linear
  container, `consume a[i]` of a linear element, the three-word layout without `$live`, the wire version
  and the addressor lowering. Windows x64 Debug: 17 of 17 fail, for the expected reasons: the role names
  are unknown (`name-resolution.invalid-prelude`), `count` is inaccessible, and the parser does not yet
  know the four new intrinsic names. (An intermediate run with the new intrinsic removed from the
  fixture's `deinit` confirmed that every other test fails on the unknown role and not on a fixture
  error.) Negative assertions require the library to load and a diagnostic that names the field or type,
  so they cannot pass because the role declaration is broken. Nothing is implemented.
- 2026-11-15: Phase 2 implemented (Windows x64 Debug). Front end: the four roles and their closed shape
  validation (a `deinit` and all three method roles required), ownership of the first type argument
  (`isLinear`, `noncopyableReason`, one whole-value leaf in `linearLeafPaths`), the subscript type, the
  read-only `count` (`Symbol::readableOutsideDeclaration`, `analyzeStorage`), the four intrinsics (parser,
  name resolution, checker; authorized in runtime and bundled prelude sources, so the role prelude is not
  made a runtime unit), the `container_end` exemption and `semantic-analysis.consume-of-linear-element`, and
  instance discovery of the addressor and (when copyable) the copy method. IR: `StructureDefinition::inertZero`
  and `copier`, `Function::elementAddressor`, five opcodes (`destroy_range`, `clone_at`, `clone_callback`,
  `destroy_callback`, `linear_range_guard`), lowering of `a[i]` to an addressor call plus `integer_address`,
  verifier rules (shapes, runtime-only, escape and dominance of the element address, addressor and copier
  signatures), wire version 20. Backend: the clone helper calls the copier, the destroy helper runs `deinit`
  unconditionally for an inert-zero struct, `destroy_range` destroys last to first. The 17 failing tests now pass
  and 16 more were added. Two reviewers (type-system, IR/backend, separate agents) found one soundness hole,
  fixed here: a struct holding a linear library array lost that field from its required leaves
  (`linearLeafPaths`). Also fixed: the escape rule rejected a role element used as the receiver of
  `array_append`, `array_remove_last`, `array_consume_empty`, `dictionary_set` and `dictionary_get`; range
  destruction order (was first to last); the copier may not consume; `clone_callback` needs a copyable type;
  the addressor and the element type of the place are verified; the role prelude stays a non-runtime unit;
  the ending hint names `finish`. `ValidationScriptTests` timed out once under parallel load and passed alone.
  Known and recorded: a library `deinit` that overwrites its whole `self` recurses (the live-flag path
  prevented that), `-O0` calls the addressor (no `alwaysinline` yet), and the addressor and copier instances are
  produced for every concrete instance. Not done: Release, Linux, macOS, `scripts/validate.py`, executed tests,
  the cost gate (Phase 3). Nothing here is pushed.
- 2026-11-15: Re-review of the fixes (type-system and IR/backend, separate agents): no soundness issue remains in
  either half; the type-system reviewer ran about 150 probe programs. Changes from it: the copier clone helper is
  `NoInline` (same debug-scope reasoning as a destroy helper that calls a `deinit`; not yet reproduced with `-g`),
  the verifier requires the addressor's receiver to have one type argument and a plain borrowed index, writing
  `count` outside the library now says why, the `consume a[i]` help mentions `joyeer_intrinsic_move_out`, and
  `clone_at` of a concrete noncopyable type is rejected where it is written. Two tests that passed vacuously were
  fixed. Open, none a soundness defect: a library `deinit` that assigns to the whole `self` recurses (consider
  rejecting it for `array_type` bodies); no test executes a library array; addressor and copy instances exist for
  every concrete copyable instance (cost, Phase 3); the layout-intrinsic name list is copied in three places; more
  coverage from the decision's list (recursive owns, `Buffer<Void>`, forgery, nested holders) is probe-verified
  but not yet pinned. Windows x64 Debug `ctest` 1751 of 1751.
- 2026-11-15: `scripts/validate.py` on commit f0ea4f9 passes. Windows x64: Debug and Release build, `ctest` 1751 of
  1751 and `fern test` 7 of 7 (`Fern.Workspace` in Debug and `Fern.Manifest` and `Fern.Workspace` in Release passed only
  on the validator's rerun, the known parallel-load timeout pattern). Linux x86_64 (WSL Ubuntu, a fresh Git clone):
  Debug and Release build, `ctest` 1727 of 1727 and `fern test` 7 of 7, no reruns. macOS is run by the user and not
  reported here. The Phase 2 review item stays open only for that and for the open items listed above.
- 2026-11-15: Phase 3 started on `feat/library-array`. Baseline of the builtin array before any change: five
  programs in `tests/perf/` (`array_append_growth`, `array_index`, `array_remove_last`, `array_nested_destroy`,
  `array_linear_drain`) and `benchmark_arrays.py`, built with the compilers of the Phase 2 validation tree (the
  merged `main` content) at -O0 (Debug compiler) and -O2 (Release compiler), checksums verified against an
  independent Python computation. Windows x64, idle machine, 15 interleaved repetitions, minimum and median in
  milliseconds. -O0: append_growth 444 / 997 (one noisy run set, expect a wide interval), index 229 / 239,
  remove_last 200 / 211, nested_destroy 166 / 178, linear_drain 135 / 147. -O2: append_growth 171 / 184, index 101 /
  112, remove_last 100 / 109, nested_destroy 102 / 106, linear_drain 134 / 151. Executable sizes: -O0 88064 to 96256
  bytes, -O2 43008 to 45568 bytes. The executables and `report.json` are kept outside the repository; the candidate
  is compared with `--candidate-dir` against a rebuilt baseline, never against these numbers alone. The linear
  drain is dominated by thread creation, so it can only show a large regression.
- 2026-11-15: Drafted [0026](../decisions/0026-library-array.md) (proposed) for Phase 3 from an inventory of the
  array machinery: the runtime already implements the array algorithms in Joyeer behind the public
  `joyeer_array_*_abi`, so the decision makes `Array<T>` a thin typed library struct over those functions (not a
  per-element-type reimplementation), declared in a third bundled prelude `collections.joyeer` so it may declare C
  bindings, with a new `array_literal` role and a place-address intrinsic, the type kind and array opcodes
  removed, and a characterization suite written first on the builtin array. The always-inliner is expected at
  `-O0` (`buildO0DefaultPipeline`), to be confirmed by a prototype. It awaits a language-designer review; nothing
  is implemented.
- 2026-11-15: Language-designer review of 0026: accept with changes. Folded into revision 2: the literal's
  ownership on early exit and a `stack_block` opcode, `array_literal` as a method role of the struct, a fully specified
  `place_address` (direct call argument only), the cost mechanism and the `NoInline` debug-scope hazard made
  preconditions with predeclared gate numbers and a generic fallback, the third prelude's seam and packaging,
  an honest residual set of array-aware code (the plan's "implement operations in Joyeer" is reworded: the
  algorithms are already Joyeer in the runtime, the deliverable is removing compiler paths), a structure `role`
  instead of extra marks, and the recursive-type, Dict, extension and Equatable risks. Phase 3 is staged as five
  changes: a characterization PR on the builtin array, a measurement PR, an infrastructure PR, the switch, and the
  cleanup. 0026 stays proposed until the measurement exists.
- 2026-11-15: Characterization PR (stage 1 of 0026), tests only, written against the unchanged builtin array and
  passing on it: eight e2e fixtures at -O0 to -O3 plus the automatic differential check
  (`array_destroy_order`: end of scope, overwrite, `?`, early return, a literal with a fallible element, a nested array
  emptied by `removeLast`; `array_element_classes`: scalars, strings, nested arrays, dictionaries, function values,
  optionals, unit values; `array_zero_state`: a moved-out field, an uninitialized `var`, an `Optional` payload, drain
  then `consumeEmpty`; `array_recursive_types`: `Tree`, a non-indirect `Json`, mutual recursion;
  `array_dictionary_interplay`; `array_constant_uses`: read, copy, task input and result; `array_evaluation_order`;
  `array_trap_order`, which pins that 7 is printed and 20 is not) and `CompilerArrayCharacterizationTests`
  (11 tests of diagnostics: type spellings, unknown member, extension rejection, `for-in`, a user type named `Array`,
  copy and linear reasons, markers and index types, `Never` and function-value elements). The two diagnostics
  0026 changes are marked `CHANGES WITH 0026` in the test. Output values were checked by hand against the language
  rules, not taken from the compiler. Windows x64 Debug: 65 of 65 pass. Findings while writing them: `[Int] == [Int]`
  type checks today and then fails in the backend with `llvm.invalid-module` (a pre-existing defect that plan 0012
  owns; not pinned as behavior); a user type named `Array` prints as `Array` today, which 0026 already lists as a
  change. Not added because fixtures already exist: native `[String]` and `[UInt8]` parameters and `main(args:)`
  (`entry_arguments`, `entry_argument_bytes`), `String.utf8()` (`string_utf8`), `[Void]` bounds (`unit_array_bounds`),
  the removeLast and consumeEmpty cases (`array_remove_last`, `array_consume_empty`).
- 2026-11-15: `scripts/validate.py` on c72d13a (the characterization PR) passes. Windows x64: Debug and Release
  build, `ctest` 1802 of 1802 and `fern test` 7 of 7 (`ValidationScriptTests`, `Fern.Manifest`, `Fern.Workspace` and
  `Compression.Interoperability` passed only on the validator's rerun, the known parallel-load pattern). Linux x86_64
  (WSL, a fresh Git clone): Debug and Release build, `ctest` 1778 of 1778 and `fern test` 7 of 7, no reruns. macOS
  is run by the user and not reported here.
- 2026-11-15: Stage 2 of 0026 (measurement), on `feat/library-array-switch`. New tools: `array-addressor-probe`
  (builds a program against a library array supplied as a source file through the role-prelude seam),
  `tests/perf/benchmark_addressor.py` and the `array_index_*` and `array_drop_*` pairs. Result: the 0025 addressor as
  merged was not good enough at `-O0` (library over builtin 1.84 [1.78, 1.89]) because it was a call and its address
  arithmetic was overflow-checked; marking it `alwaysinline` gave 1.47, and a new unchecked layout intrinsic
  `slot_address` (one opcode, wire version 21) plus one comparison per `if` gave 1.10 [1.05, 1.11] at `-O0` (1.12 with
  full debug information) and 0.94 at `-O2`. Create-and-drop is 0.95 and 0.88, so the `NoInline` helper is not a
  measurable cost. 0026 Section 4 records the table and fixes the gate numbers; the decision awaits the user's
  acceptance. The alias-information mapping and narrowing instance discovery were judged unnecessary for now.
  Windows x64, idle machine, 25 repetitions.
- 2026-11-15: Reviews of the measurement change (language designer on 0026, IR/backend and type system on the code,
  separately). No soundness issue in the code. The 0026 review gave accept with changes and found the previous log
  entry overclaimed: the drop pair was not like for like (a different allocation path and `deinit`), so "not a
  measurable cost" did not follow, and the `-O0` gate had been loosened from 1.10 to 1.15 after measuring without saying
  so. Changes: `slot_address` is a plain GEP (no `inbounds`, so an address never loaded through is not poison); the
  prototype library array now uses the runtime create function, the real header and 0026's `deinit`; the tests pin the
  element GEP and per-instance lowering, cover the verifier's malformed forms and the wire round trip, and
  `tests/c-interop/layout-intrinsics.joyeer` checks the arithmetic; §20.12.5 states the contract. Remeasured (Windows
  x64, 25 interleaved repetitions, 50 for one rerun): `index` 1.11 [1.09, 1.13] at `-O0` and 0.96 [0.93, 0.99] at
  `-O2`; `drop` 1.06 to 1.09 at `-O0` and 1.00 at `-O2`. The builtin baseline was re-recorded with 50 repetitions;
  `array_append_growth` at `-O0` is now tight (437.7 / 450.5 ms) and is gated. 0026 revision 3 proposes an explicit
  `-O0` amendment of 0025's gate (median at most 1.15, revision 2 had proposed 1.10) with size, compile-time, rerun and
  missed-gate rules; it needs the user's acceptance.
- 2026-11-15: The user accepted 0026 revision 3 "for now", including the `-O0` gate amendment (median at most 1.15),
  and asked to implement first; the decision is revisited if the switch's measurement contradicts it.
- 2026-11-15: 0026 stage 3 (infrastructure), Windows x64 Debug. A third bundled prelude, `lib/Runtime/collections.joyeer`
  (empty until stage 4; a runtime unit like the concurrency prelude), is embedded by the CMake template, parsed by
  `parsePrelude` and loaded after the concurrency and role preludes with source identities max+1, +2 and +3 (each only
  when bundled); the driver's source-table check needed no change, and two tests that hard-coded the table size
  (`ModuleServiceTest`, `IRLoweringTest`) now expect the third prelude. The test seam `ModuleInput::rolePreludeContext`
  is now the enum `ReplacedPrelude` (`none`, `roles`, `collections`). New layout intrinsics (spec 20.12.5, wire
  version 22, all authorized like the others): `joyeer_intrinsic_stack_block<T>(count: N)` (one opcode; a literal
  count; storage allocated in the prologue; 0 for none), and `joyeer_intrinsic_place_address<T>(value: &place)`
  (accepted only as a direct argument of a call, which holds the `inout` access, so a sibling argument that touches the
  place is `overlapping-access`; no new opcode). The fourth method role `array_literal` (`consuming func
  name(address: Int, count: Int): Self`, closed shape, required and unique, `self` exempt from the linear-not-consumed
  check) has no user yet: its literal lowering and instance discovery belong to stage 4, where a literal first
  produces a role array. An element addressor now has internal linkage. Tests: `CompilerCollectionRoleTests` 47 of 47
  (three-prelude identities, replacing the collections prelude, a supplied concurrency prelude, `stack_block` and
  `place_address` forms and verifier cases, the `array_literal` shape), and the stage fixture
  `tests/c-interop/layout-intrinsics.joyeer` runs `slot_address`, `stack_block` and `place_address` at -O0 to -O3.
  Not done in this commit: the cost gate (stage 4), Release, Linux and macOS.
- 2026-11-15: `scripts/validate.py` on 7a1d748 (stages 2 and 3) passes. Windows x64: Debug and Release build, `ctest`
  1815 of 1815 and `fern test` 7 of 7 (`Fern.Workspace` in Debug and `ValidationScriptTests`, `Fern.Manifest` and
  `Fern.Workspace` in Release passed only on the validator's rerun, the known parallel-load pattern). Linux x86_64
  (WSL, a fresh Git clone): Debug and Release build, `ctest` 1791 of 1791 and `fern test` 7 of 7, no reruns. macOS is
  run by the user and not reported here.
- 2026-11-15: Stage 4 prerequisite on macOS/AArch64, using the existing LLVM 22.1.8 SDK and macOS 26.5 SDK.
  The unchanged stage 3 runtime build aborted because Mach-O cannot lower the shared static-slot table's COMDAT.
  `CInteropTest.LowersStaticSlotsToAMergedTableWithARangeCheck` was changed first to require no COMDAT on macOS
  and observed failing. The backend now retains `linkonce_odr` coalescing on every platform but attaches a COMDAT
  only on object formats that support it. The same test passes, the runtime builds, and the four
  `JoyeerCInterop.StaticSlots{Debug,Optimized}` / `SharedStaticSlots{Debug,Optimized}` execution tests pass.
  The unchanged array characterization (11 tests) and collection-role baseline (47 tests) also pass. This is not
  full macOS acceptance or evidence that the library-array switch is implemented.
- 2026-11-16: Stage 4 implementation checkpoint, macOS/AArch64. `[T]`, literals, native byte/string-array
  marshalling, `String.utf8()` and constants bind to the array role; nominal `Array` stays shadowable.
  The real library implements append, removal, ending, copying, bounds checks and destruction over the
  unchanged runtime ABI. All owned literal elements are evaluated before one verified packing/transfer
  sequence. `StructureRole::array`, literal-factory and wrapper-inlining metadata use wire 23.
  Tests were observed failing before the switch for role identity, literals, missing roles, private fields,
  shadowing, owned-argument compatibility and wire version. Additional coverage includes malformed literal
  address graphs, a 1000-element literal, `Void`/`Never`, expanding role recursion, C size/offsets and Joyeer
  size/alignment, and `array_task_transfer` (linear handle arrays as task input and result).
  The unchanged eight characterization fixtures pass at O0-O3; only the two designated diagnostic assertions
  changed. Ordinary compiler-stage assertions now describe library calls and role structs.
  A language-designer review approved the owned-element argument compatibility clarification and required the
  closed literal initialization protocol, which is implemented. No new public syntax was added.
- 2026-11-16: Stage 4 platform follow-through. The full macOS Release build and unfiltered CTest pass 1809/1809,
  including the final wrapper-inlining adjustment. Two initially failing native callback probes exposed a second
  macOS prerequisite: `-force_load` pulled every runtime archive member, conflicting with their test channel
  exports. Normal lazy archive loading fixes both and preserves optional thread/channel objects.
  The C ABI test now requests wire 23. Whole-self assignment in a `deinit` was already rejected by the existing
  checker; a durable library-role regression now pins that rule. The parser reuses the layout-intrinsic name helper.
- 2026-11-16: Initial switch measurements on an idle macOS/AArch64 host, LLVM 22.1.8, 25 interleaved pairs
  per program/profile, retained outside the repository. The immutable builtin baseline includes only the
  Mach-O COMDAT prerequisite. Initial append O0 was 1.161 [1.155, 1.164]; marking role-owned element wrappers
  always-inline reduced it to 1.000 [0.997, 1.003]. Internal runtime/helper linkage removes unused code;
  `NoInline` is now needed only with debug information. The latest candidate's O0 paired ratios are append
  1.000, index 1.071, removal 1.064, nested destruction 1.102, linear drain 0.995; O2 ratios are 1.000, 0.848,
  0.966, 1.022, 1.004. Nested destruction remains outside sampled O2 noise [1.008, 1.028] and grows 16432
  executable bytes because its extra text/unwind data crosses a 16 KiB Mach-O segment boundary. Other O2
  executables change by 0 to -288 bytes. These are preliminary macOS results, not a passed acceptance gate:
  0026 gates Windows x64, which is unavailable in this session, and requests Linux reporting. Final measurements,
  the one allowed doubled rerun of failing configurations, reviewers, cleanup and Debug/Release validation remain open.
- 2026-11-16: Stage 5 and review fixes. The dedicated array type kind/opcodes, their builtin method dispatch,
  and backend array layout/access/ownership branches are removed. Generic role capabilities, constant
  headers, native marshalling and `String.utf8()` remain. The removed numeric slots are held.
  The independent IR/backend review found two malformed-IR/wire gaps: a factory function value could reach
  `call_indirect` without packing verification, and role 257 narrowed to valid uint8 role 1.
  Both new tests were observed failing first. Factory function values are now rejected, and enum/unsigned
  wire values are range-checked before narrowing. Independent re-review confirmed both fixes.
- 2026-11-16: The actual product drop benchmark exposed redundant zero-state owners that the earlier Buffer
  prototype did not include (O0 about 1.45, O2 with debug about 1.31). Literal/copy output now uses a typed raw
  slot, moved out once; the range guard still precedes a zero-address return in deinit. Closed literal/copy/deinit
  wrappers and their referenced helpers inline by role/declaration policy. Artificial helper `DISubprogram`s
  and unknown-line implicit call locations replace the `NoInline` workaround. The scope test failed first;
  native gfull execution and DWARF verification pass. Independent IR follow-up found no correctness issue.
- 2026-11-16: An additional baseline comparison found `[Never].append(element: return)` compiled with the
  builtin but failed by demanding an uninhabited generic instance after the switch. Calls with a statically
  diverging argument remain fully type/constraint checked but do not demand a callee instance; lowering still
  evaluates earlier operands with their normal ownership before stopping. Tests failed first and now pin
  destruction, skipped later arguments, existing instances and generic-caller substitutions.
  A separate runtime-unit empty-Never-literal failure was also reproduced and fixed: factory-associated
  packing blocks get the same closed verification in all contexts, without relaxing ordinary raw-block rules.
  The type-system follow-up and IR-context follow-up found no remaining issue in these changes.
- 2026-11-16: Final review/validation evidence. The type-system follow-up checked role binding, declared-T
  owned argument contexts, constraints, divergence and substitutions. The adversarial follow-up ran 18 native
  executions and three validation attempts (return/propagation, nested moves/copies, Void/Never, task arrays,
  nonempty ending traps and gfull equivalence) with no new findings. The full validator passes on the final
  macOS/AArch64 candidate: Debug and Release builds, unfiltered CTest 1823/1823, and fern test 7/7, no reruns.
  The first fern run exposed an unrelated order-sensitive System test, reproduced with the preserved builtin
  compiler; the assertion now accepts either exact two-name order, matching spec 18.2's unspecified enumeration
  order. No filesystem implementation behavior changed.
- 2026-11-16: Final macOS measurements on the reviewed candidate, idle host, 25 interleaved pairs per
  configuration and one 50-pair rerun of failing comparisons; all raw samples are retained in session artifacts.
  In append/index/removal/nested-destruction/linear-drain order, O0 paired ratios are
  1.000/1.073/1.070/1.067/0.998; O2 are 0.747/0.850/0.964/0.994/1.010.
  All five O2 runtime comparisons are within or better than sampled noise. Nested destruction still adds
  16432 executable bytes (a 16 KiB Mach-O segment boundary); its 50-pair runtime rerun is
  0.994 [0.985, 1.001]. All other O2 executable changes are 0 to -288 bytes.
  Flat index pairs in none/lines/full debug modes are O0 1.066/1.066/1.064 and O2 0.807/0.805/0.808.
  Flat drop is O0 1.066/1.061/1.065; the O2 doubled reruns are
  1.019 [1.016, 1.023], 1.019 [1.016, 1.021], 1.021 [1.018, 1.025], still a small regression outside sampled noise.
  Hello O2 is 69488 vs 69536 bytes; its emit-LLVM frontend median ratio is 1.032 [1.021, 1.041].
  Type-ID-normalized unoptimized LLVM IR differs as expected because source wrappers/role structs replace
  builtin operations; all benchmark outputs and exit statuses match.
- 2026-11-16: Acceptance remains open, not silently waived. Section 4 gates Windows x64, which this macOS
  session cannot execute; final Windows/Linux validation and Windows gated performance measurements are unrun.
  The macOS drop/noise and nested executable-size comparisons above are recorded failures against those
  numerical limits, not claimed passes or a gate amendment. No push or PR is authorized until the required
  post-commit personal testing/understanding/full-diff review confirmation; LLM use must be disclosed.
- 2026-11-16: Review and validation of the pushed switch (656ac2e) on Windows x64 and Linux x86_64.
  `scripts/validate.py` passes on both. Windows: Debug and Release build, `ctest` 1852/1852 in each (flaky
  `Fern.Workspace`, `Fern.Manifest`, `Compression.Interoperability` passed on rerun), `fern test` 7/7. Linux:
  `ctest` 1828/1828 in each configuration, `fern test` 7/7. Independent IR/backend and type-system reviews found
  no soundness or ABI issue (33 and about 110 differential programs against the builtin baseline, identical
  output). Two low findings were fixed with the failing tests committed first (6dcfbee, a4419d3): a literal as a
  member base resolved `Array` by spelling, and the linear help named `consumeEmpty` for nested or wrapped arrays.
- 2026-11-16: Windows x64 gated measurements (section 4 of 0026). Idle machine, LLVM 22.1.8, baseline = Release
  compiler built from 7a1d748 (the builtin array), candidate = Release compiler built from d1cf2ca, both
  compiling every program to `-O0`/`-O2` with `-g0` unless a debug mode is named; 25 interleaved pairs per
  configuration, paired median ratio with a 95% bootstrap interval (raw reports under `out/perf-gate`, not kept
  in the repository). Five `array_*` programs, O0 ratios: append 1.024, index 1.145 [1.116, 1.160], removal
  1.002, nested destruction 0.998, linear drain 0.989; O2: 1.010, 0.997, 1.015, 1.019, 1.006, every lower bound
  at most 1. Flat pairs, none/lines/full: index O0 1.093/1.098/1.086, O2 0.982/0.982/1.022; drop O0
  1.063/1.061/1.044. Flat drop O2 first run missed in two modes (none 1.034 [0.939, 1.061], full 1.034
  [1.006, 1.092]); the one allowed 50-pair rerun passes (none 1.022 [0.998, 1.051], full 1.005 [0.982, 1.038]),
  and lines was 1.016 [0.951, 1.036]. Executable growth: at most +3584 B at O0 (nested destruction) and
  +1536 B at O2 (nested destruction); `hello` O2 43008 B against 43008 B. Hello `--emit-llvm` front end, 60
  interleaved pairs, both compilers copied to equivalent directories: 1.001 [0.975, 1.019] (an earlier run with
  the compilers in different directories was dominated by a location effect, 154 ms against 47 ms, and is not
  used). Result: the Windows numerical gates pass; the macOS drop and nested-size misses did not reproduce.
  The index O0 median is within 0.005 of its 1.15 limit. Linux performance is not measured. Acceptance of the
  amendments to 0026 and the final go-ahead remain the user's.
- 2026-11-16: `scripts/validate.py` on 71db2ec (the pushed switch plus the review fixes, the doc corrections and
  revision 4 of 0026) passes. Windows AMD64: Debug and Release build, `ctest` 1854/1854 in each (`Fern.Manifest` and
  `Fern.Workspace` passed only on rerun), `fern test` 7/7. Linux x86_64: Debug and Release build, `ctest` 1830/1830
  in each, `fern test` 7/7, no reruns. Open follow-ups accepted with revision 4: debug-info verification after the
  optimization pipeline and an array fixture at `-O1`/`-O2` with `-gfull`, the spec text of the diverging-call rule,
  stricter verifier mutation tests, and the `[Never]` compiler crashes (closed by 0026 amendments A10, arrays, and A11, dictionary keys and values).
- 2026-11-16: `scripts/validate.py` on e1a4951 (the switch plus review fixes and 0026 amendments A10 and A11, which
  reject `Never` as an array element and as a dictionary key or value) passes. Windows AMD64: Debug and Release
  `ctest` 1857/1857 in each (`ValidationScriptTests`, `Fern.Manifest`, `Fern.Workspace`, `Compression.Interoperability`
  passed only on rerun), `fern test` 7/7. Linux x86_64: Debug and Release `ctest` 1833/1833, `fern test` 7/7, no
  reruns. Phase 4 started: [0027](../decisions/0027-library-dict.md) drafted (proposed, not yet reviewed).
- 2026-11-16: [0027](../decisions/0027-library-dict.md) (library `Dict`) accepted by the user for stages 1-4 at
  revision 4. Next: stage 1, the characterization tests on the builtin dictionary.
- 2026-11-16: 0027 stage 1 (characterization), tests only, on the builtin dictionary.
  `CompilerDictCharacterizationTests` (15 tests) pins the diagnostics, three marked `CHANGES WITH 0027`
  (non-admitted keys pass the checker; `d.address` is an unknown member; a user `Dict` captures `[K: V]` member
  lookup) plus `==` type-checking (checker only: compiling it asserts in a Debug LLVM build). Five e2e fixtures at
  O0-O3: `dict_ownership_order` (replacement, duplicate-key literal, destruction order), `dict_set_order` (key then
  value, `?` inserts nothing, growth from the right-hand side, a key read from the same dictionary, nested
  assignment, a value moved from another entry), `dict_library_edges` (`Void` values, a dictionary in an array, a
  recursive type, a task input), and two traps, `dict_trap_order` and `dict_nested_missing_trap` (`CHANGES WITH
  0027`: the trap precedes the right-hand side today). Expected outputs were derived from
  `lib/Runtime/core/dictionaries.joyeer` before running; all 40 matching tests pass on Windows x64 Debug. Not yet
  pinned: the backend-only failure of a used non-admitted key (`llvm.unsupported-type`, no span) and the allocation
  counter.
- 2026-11-17: `scripts/validate.py` on 8aed3ea (0027 stage 1) passes: Windows AMD64 Debug and Release `ctest` 1897/1897
  (`Fern.Workspace`, `Fern.Manifest`, `Compression.Interoperability` passed only on rerun), `fern test` 7/7; Linux
  x86_64 1873/1873 and 7/7, no reruns. The validated Release compiler is preserved as the builtin-dictionary baseline in
  `out/baseline-builtin-dict` (not in the repository).
- 2026-11-17: 0027 stage 2a (role-table refactor, no behavior change). The checker's four-slot `arrayRoleMethods`
  table becomes a `CollectionRole` record (structure role, owned type parameters, method-role map); method-role
  registration and the owner check are table-driven (`collectionOwnerRole`, `requiredCollectionMethodRoles`); the
  owned-argument rule reads the record's owned parameters; the IR verifier and the copy-dependency graph take the
  element type argument from the structure role (`collectionElementArgument`). Evidence: 482 matching tests pass on
  Windows x64 Debug, and the LLVM IR of 24 array, dictionary, task and hello fixtures at O0 and O2 is byte-identical
  to the baseline compiler's (48 of 48). Deliberately left for stage 5, where the dictionary roles give them a second
  case and tests: the per-role shape checks and messages, the `index`-typed addressor shape, the literal-factory and
  packing verifier, the single-array-symbol checks in `ir.cpp`.
- 2026-11-17: Stage 2a reviews. Type-system and IR/backend reviewers each found no behavior change (the IR reviewer
  re-ran 46 compiles, byte-identical). Follow-ups applied: the method-role owner check fails closed for a role missing
  from `collectionOwnerRole`; a stale comment; two verifier mutations (an array-role structure with two or with no
  type arguments). Deferred to stage 5, where a second collection role exists: owned parameters taken from a per-role
  table rather than all type parameters, and role names in the shape messages.
- 2026-11-17: `scripts/validate.py` on bf5f58c (stage 2a) passes: Windows AMD64 Debug and Release `ctest` 1897/1897
  (`Fern.Manifest`, `Fern.Workspace` passed only on rerun), `fern test` 7/7; Linux x86_64 1873/1873 and 7/7.
- 2026-11-17: 0027 stage 2b. Tests first (9e678e1, observed failing: the parser did not know either name):
  `BorrowAddressIsAuthorizedAndOnlyADirectArgumentOfANamedPlace`, `KeyKindIsAdmittedOnlyForTheFourKeyTypes`, and the
  stage fixture `tests/c-interop/layout-intrinsics.joyeer` (borrow_address of a `let`, a field, a borrowing parameter
  and a `String` handle; the four key kinds; the `Entry<K, V>` layout equal to the runtime's entry formula for every
  admitted key against `Void`, `UInt8`, `Bool`, `Int`, `String`, a padded struct, `Int?`, `[Int]` and an enum).
  Implemented `joyeer_intrinsic_borrow_address` (direct call argument, named place, a read access for exclusivity,
  lowered like `place_address`) and `joyeer_intrinsic_key_kind` (concrete non-admitted types rejected by the checker,
  instances by lowering, never 0). Evidence: collection-role tests 69/69; the stage fixture passes at O0 with
  `-gfull`, O1, O2 and O3; 484 matching tests pass on Windows x64 Debug; existing fixtures' LLVM IR is byte-identical
  to the baseline (48/48). Spec §20.12.5 has the two rows.
- 2026-11-17: Stage 2b reviews. IR/backend: lowering, IR and backend sound for every place kind (spilled parameters,
  locals, field paths, String handles, `self` in each receiver mode) at O0 with `-gfull` through O3; `key_kind`
  constants substitute per instance, nested generics included. Type-system: revise. Both found, and f1cce50 (tests,
  observed failing) and f3e0223 (fix) close: the place's type must equal `T` exactly (optional promotion described an
  `Int?` layout over an `Int`; `place_address` had the same pre-existing gap); `borrow_address` takes only a local, a
  parameter or a field path (subscripts, computed members, top-level constants and enum cases were accepted or failed
  late); neither intrinsic may be an argument of an initializer or enum case, which stored the address; `key_kind`
  reports `Never` once and rejects `[K]` in the checker. Collection-role tests 70/70; 485 matching tests pass. The
  intermediate commit ae17955 has a stage fixture that does not compile (fixed in f1cce50). Still open, by design or
  low: an address returned through an identity call (the author's duty, as for `place_address`), writes through the
  read-only address unchecked, the `key_kind` lowering error names no instantiation site, and the key-kind table exists
  in the checker, lowering and the backend (the backend copy goes at the cleanup stage).
- 2026-11-17: Stage 2b head 0f937ac validated: Windows AMD64 Debug and Release `ctest` 1900/1900
  (`E2ECheckedArrayEmptyO0` in Debug and `Fern.Workspace` in Release passed only on rerun), `fern test` 7/7; Linux
  x86_64 1876/1876 and 7/7, no reruns.
- 2026-11-17: 0027 stage 3 (measurement), Windows x64, idle machine, 25 interleaved repetitions, paired bootstrap.
  The benchmarks of 3baf1cd were too small: process startup is about 90 ms here and every `-O2` total was within a
  few ms of `hello`, so a first run measured process creation and is discarded. The rounds are scaled (×10 lookup,
  ×50 `String` lookup, ×100 growth and literal, ×200 nested destroy; 0.3-0.5 s at `-O2`, 1.2-1.9 s at `-O0`) and the
  checksums recomputed from closed forms. Noise (builtin against builtin): every median within 0.993-1.018, every
  interval contains 1. Prototype: the role-less `Table<K, V>` of `tests/perf/dict_table_prelude.joyeer`
  (`borrow_address`, `key_kind`, the runtime dictionary ABI) through `array-addressor-probe`, explicit methods,
  `benchmark_dicts.py prototype`. Paired median ratios, Table over builtin:

  | Program | `-O0` | `-O2` [95% interval] | size `-O0`/`-O2` |
  |---|---|---|---|
  | lookup (`Int`, sizes 1-64, `at` and `get`) | 1.039 | 0.997 [0.987, 1.004] | +1536/+0 |
  | `String` lookup | 1.031 | 1.009 [0.986, 1.027] | +2560/+512 |
  | insert with growth and replacement | 1.051 | 0.998 [0.994, 1.018] | +1536/+0 |
  | nested destroy `[String: [Int]]` | 1.109 | 1.050 [1.035, 1.066] | +4096/+512 |

  Verdict as measured: **no-go on nested destroy** (both profiles over the 1.05 `-O0` margin and the `-O2` gate);
  insert growth at the margin (1.051); lookup and `get` go. Known non-like-for-like costs, all against the
  prototype: without a role `Table` carries the hidden live flag (a 32-byte value and a flag test per drop; the role
  type is a 24-byte inert-zero handle); its methods are not always-inlined at `-O0` (0026 A4 applies by role); `at`
  returns `V` by copy where `d[k]` reads in place. Attribution: a builtin variant that also copies the two `[Int]`
  values out (`let first = copy[k]`) reduces the nested-destroy ratio to 1.060 [1.054, 1.068] at `-O0`, but leaves
  1.062 [1.057, 1.071] at `-O2`, so about 6% is unexplained; the builtin does clone the dictionary
  (`joyeer_dictionary_clone_abi`), so the copy is like for like. Raw reports in `out/perf-gate/dict-stage3b` (not in
  the repository). Next: attribute the remaining nested-destroy cost before stage 5.
- 2026-11-17: Stage 3 attribution of the nested-destroy miss (Windows x64, 21 interleaved repetitions each).
  Cumulative phase pairs of the nested-destroy program (build and drop; plus the copy; reads without the copy) put
  the whole `-O2` gap and most of the `-O0` gap in building the `[String: [Int]]` table: 109 ns per round at `-O2`
  (796 at `-O0`), with the copy adding nothing. The optimized IR shows why: each `Table.set` drops two moved-from
  Strings (the consumed `key` parameter and the caller's `inserted` local). A moved-from String is the zero value,
  but its destroy is the opaque runtime call `joyeer_string_destroy_abi`, which then calls `release` with address 0,
  so 32 needless calls per round; a moved-from array costs nothing (its `deinit` is inlined and the zero test
  folds), which is why the `Int`-keyed rows show no gap. The builtin `&d[k] = v` moves the key into the runtime with
  no drop. Experiment 1 (reverted; patch kept in `out/perf-gate/dict-stage3b`): the String destroy helper tests
  `data` inline and calls the runtime only when it is not null (behavior-preserving: every owned String has a block,
  and `release` ignores 0). The build and copy phases then match the builtin (`-O0` 1.002 and 1.013, `-O2` 1.006 and
  0.999), and the full program reaches 1.001 [0.984, 1.008] at `-O2`, but stays at 1.059 at `-O0`, the same against a
  builtin that also copies the values out (1.055). Experiment 2 (reverted): additionally always-inline `Table`'s
  methods and `emptyTable`, as 0026 A4 does for the role wrappers. At `-O0` with both: lookup 1.012, `String` lookup
  1.022, insert growth 1.010, nested destroy 1.032 [1.020, 1.039], all inside the 1.05 margin. Verdict: **go**, with
  two conditions for the switch: (1) the moved-from String drop must cost no runtime call (the inline guard, or
  static drop elision) — the role's `dict_set` consumes its key exactly as `Table.set` does, so the switch would
  carry the same cost otherwise; (2) the dictionary role wrappers join the always-inline policy (already in 0027,
  "Cost mechanism"). The remaining `-O0` cost of nested destroy (about 3%) is `at` returning `V` by copy, which
  `d[k]` through the role addressor does not do.
- 2026-11-17: Switch condition (1) of the stage 3 verdict landed: the `String` destroy helper
  (`compiler/backend/native_ownership.cpp`) tests `data` inline and calls `joyeer_string_destroy_abi` only when it is
  not null. Behavior-preserving: a null `data` owns no block (every allocating producer stores a non-null block;
  `release` ignores 0). Tests first (caea1bc, observed failing: the helper was a bare call):
  `StringDestroyHelperCallsTheRuntimeOnlyForANonNullBlock` (the non-null edge reaches the call) and the
  `string_moved_drop` fixture at O0-O3 (empty Strings freed directly, through an array and through a dictionary's
  stored callback; moved-from parameters, locals, a struct field, array elements and enum payloads). Two
  debug-location tests count one more `br i1`, the helper's, which carries `!dbg`. IR/backend review: accept, no
  high or medium findings; its low findings applied (the invariant's wording, the fixture's true moves, the branch
  shape); the count tests stay module-wide.
- 2026-11-17: `scripts/validate.py` on b738906 (the String destroy guard) passes: Windows AMD64 Debug and Release
  `ctest` 1906/1906 (`Fern.Workspace` in Debug and `Fern.Manifest` in Release passed only on rerun), `fern test` 7/7;
  Linux x86_64 1882/1882 and 7/7, no reruns.
- 2026-11-17: 0027 stage 5 (infrastructure part 3: the dictionary roles), `[K: V]` and literals untouched. Tests
  first for each step, all observed failing: 2477086 (roles and shapes: unknown role names), 4ebb54f (IR, wire,
  backend: the table lowered with no role and a live flag), 3da63de (literal protocol: the verifier admitted only
  array storage), 7d599fe (review findings). Implemented: dac12cc, the seven roles parsed, recorded through the seam
  (`dictSymbol_`, not required until the switch) and validated by the collection-role path shared with the array
  (two owned parameters, the three private words, a deinit, each method role once with its closed signature, every
  one private but `get`, a private entry with exactly `key: K` then `value: V`), linearity and copyability over both
  arguments, the affine-only rule, no extension, instance discovery of the addressor, `dict_set`, the copy and the
  literal method, and the interned entry; eac28ee, the IR flag `arrayLiteral` renamed `literalFactory`; 66e756f,
  `StructureRole::dict` and `dictEntry`, wire 24, the inert-zero destroy, the always-inline wrappers by role, and
  the backend's entry-layout invariant; 7b87f96, the closed packing protocol of a dictionary literal, checked rule
  by rule (0027 rules 1-6) and naming the rule that fails, against a literal built by hand in the form the switch
  will lower; 2b7b9da, docs and spec §20.12.5. Three test bugs were corrected in their own commits (c2b2962,
  4ebb54f; a renamed label also renamed the body's parameter, a removed entry struct also removed a type the
  literal names, an affine table may drop its self, a struct key cannot be lowered).
- 2026-11-17: Stage 5 reviews, separately from the author. Type-system: no high; mediums fixed in 06d4d41 (tests
  7d599fe): a type record read after `isCopyable` could grow the table (the stale read did not fire in the pinned
  scenario), `get` of a noncopyable value now rejected at the call by the callee's role, `dict_get` must be named
  `get`; lows fixed: no transfer marker on either struct, argument copies before recursion, a vacuous needle. IR/
  backend: no high, no malformed literal accepted; mediums fixed: an entry is never a value (no value, parameter,
  result, field, payload or type argument of its type, no capability opcode naming it, its address only a verified
  literal place), and a packed literal now reaches LLVM at O0 and O2; lows fixed: the layout check runs after every
  body is set and also compares the portable layout model, no clone or destroy helper for an entry, a cross-block
  transfer is rule 6, a stored value has exactly one use, the stale array wording. Deferred to stage 6 (the switch),
  where lowering consumes them: the entry's field symbols in the role record, the `invalid-prelude` seam at first
  dictionary use, a durable stage fixture (no user path reaches the roles yet), executing a packed literal, and
  per-instruction noise after a failed literal. Left as is: one mistake in the owner's parameter count is also
  reported by each method role; the array protocol does not require single-use stored values (pre-existing; the
  source lowering never reuses one). Evidence: dictionary-role tests 26/26, collection-role tests 70/70, Windows x64
  Debug `ctest` 1932/1932.
- 2026-11-17: `scripts/validate.py` on a4cff1b (stage 5 with its review fixes) passes: Windows AMD64 Debug and Release
  `ctest` 1932/1932 (`Fern.Workspace` in Release passed only on rerun), `fern test` 7/7; Linux x86_64 (fresh WSL
  clone) 1908/1908 and 7/7, no reruns.
- 2026-11-17: 0027 stage 6 (the switch), in progress. Debug-scope prerequisite (593e049 tests observed failing,
  be7bd70): a test build verifies the optimized module and its debug info after the LLVM pipeline (0026 known gap
  (a)), proven by a test-build fault injection; `dict_debug_scopes` runs at -O1 and -O2 with `-gfull`. Key admission
  (4360528 observed failing, 733f2ed test fix, 939d210): keys outside `Int`, `Bool`, `UInt8`, `String` are rejected at
  the type, once per mistake. A pre-existing gap found on the way, not fixed: a parenthesized function-type key
  (`[((Int): Int): Int]`) escapes the key checks. The switch (c6420f2 tests observed failing, d20be37 test fixes,
  2593f55): `[K: V]`, literals and `Dict` bind to the bundled library `Dict<K, V>`; subscripts, `dict_set` (value
  before the base address), literals through the packing protocol and `get` lower through the roles; the visible
  changes of 0027 are pinned. Spec and docs ddaecf7. Evidence: Windows x64 Debug `ctest` 1940/1940.
  Differential against the builtin baseline (939d210, same build options, Debug compilers at -O0/-O1 and Release at
  -O2/-O3): 23 fixtures that mention a dictionary, 92 runs, identical exit codes, stdout and stderr except the
  documented `dict_nested_missing_trap` (`rhs` before the trap); live-allocation counts identical at every checkpoint
  and level (ceb3c07 pins them, and that a lookup never copies its key).
- 2026-11-17: 0027 stage 6 cost gate, Windows x64, baseline 939d210 against the candidate, both built with the same
  options; 25 interleaved repetitions, paired bootstrap. Sizes pass (at most +2048 bytes at -O0, +512 at -O2; `hello`
  +0 at -O2); `hello` compile time 0.994 [0.951, 1.009]; every -O0 row passes (at most 1.074). First run at -O2:
  lookup 1.063 [1.054, 1.087] failed, insert growth 1.007 [1.004, ...] and literal 1.014 [1.006, ...] failed the
  interval rule. The lookup cost was the library `get`: its two returns left an aggregate phi after inlining that a
  caller's `match` did not fold; 051274b returns one local, which SROA splits (the builtin's shape). Second run, all
  rows: -O0 within 1.006-1.060; -O2 lookup 1.020 [0.999, 1.040], insert growth 1.004 [0.999, 1.019], literal 1.022
  [1.000, 1.030], nested destroy 0.975, and `String` lookup 1.039 [1.027, 1.068] (1.002 in the first run). Rerun
  rule (50 repetitions): `String` lookup -O2 1.038 [1.029, 1.050], **the gate is missed**. Attribution: the optimized
  hot loops are the builtin's at the IR level (same runtime calls and key spill, plus lifetime markers); an A/A
  layout probe of the baseline (one unused function) gives 1.000 [0.985, 1.009]; split variants give strict reads
  0.990 [0.980, 1.016] and `get` 1.014 [1.006, 1.020], which do not add up to the whole. Per 0026's rule the switch is
  blocked on this row until it is optimized or the user decides. Raw reports in `out/perf-gate/dict-switch`.
- 2026-11-17: Machine-code investigation of the `String` lookup -O2 miss (user's choice). The two executables' hot
  loops are the same instructions (the `at` and `find` call sequences, the 16-byte key spill, the checked add; only
  the spill's stack slot differs), confirmed by disassembling the real executables against `clang -O2` of each
  compiler's `--emit-llvm` output. What differs is placement: the candidate's larger user code puts the runtime library
  0x90 bytes later, so `joyeer_dictionary_find_abi` sits at offset 0x50 instead of 0xc0 within its 256-byte block,
  and the runtime's comparison scan with it. Unused user functions shift that placement exactly (`layout.py`).
  E1, the builtin baseline padded so its runtime lands at the candidate's addresses, against the unpadded baseline:
  1.042 [1.013, 1.057], the miss itself. E2, the candidate padded so the runtime has the baseline's offsets within a
  64-byte line, against the baseline: 0.990 [0.981, 1.001], within the gate (50 repetitions each). So the row's
  difference is the runtime's code placement, which any change in user code size moves; the library dictionary
  itself costs nothing measurable there. Raw reports in `out/perf-gate/dict-switch/layout2`.
- 2026-11-17: The user accepted the `String` lookup -O2 row on the placement evidence (recorded in 0027's acceptance
  condition; the gate limits are unchanged). The switch proceeds to the role reviews and full validation.
- 2026-11-18: 0027 stage 6 role reviews (type-system and IR/backend, 53b538c..ffb0fd3), two re-review rounds, every
  fix test-first (tests f262f9d, e1b6c2e, 04db019 observed failing; fixes 680b42a, 4ba2e53, 07d2fac). Fixed: a
  nested target's String key borrowed across a right side that consumed or reassigned it (use after free, new with
  the switch), and the same for a read whose index disturbs its base (a grown outer table, and a String byte read
  through a binding, field, dictionary value or array element, older than the branch): key operands are owned and
  the base is addressed only after any operand that may disturb it; a plain lookup still copies no key. Verifier
  rule 4 follows paths from the packing (join and loop shapes rejected; each walk pinned by a mutation test). The
  post-optimization verify and its fault injection move behind `JOYEER_BACKEND_TEST_HOOKS` (Debug presets and
  `validate.py`), out of Release packages. A right side that consumes the target's root is rejected (older than
  the branch) and one that initializes it is pinned (spec 4.4.3); one key diagnostic per mistake, including under an
  optional context, which now gives array and dictionary literals their types; `Dict<Never, V>` reads like
  `[Never: V]`; dictionary help for `count`; only `Dict`'s own key parameter is exempt in a prelude; an unannotated
  literal reports a missing `dict_type` role. The benchmark programs compile to byte-identical LLVM IR before and
  after these fixes at -O0 and -O2, so the cost gate stands. Correction to the 2026-11-17 entry: `[((Int): Int): Int]`
  is an array of functions, not an escaping key; the real issue is that `[(Point): Int]` displays like a dictionary
  type. Left open, older than the branch: compound assignment analyzes its index twice (`&a[take(i: consume i)] += 1`
  reports use-after-consume); a key under a generic context (`Box(d: [[3]: 4])`) reports twice; compile time grows
  about 14x from 100 to 200 blocks in one function (arrays too); an operator index (`d[k][i + 1]`) copies a String
  key it could borrow. Validation at 07d2fac: Windows AMD64 Debug and Release `ctest` 1971/1971, `fern test` 7/7
  (six tests passed only on rerun while the machine ran reviewer timing jobs: Fern.Workspace, Fern.Manifest and four
  `E2EOptimizationEquivalence_*`); Linux x86_64 fresh WSL clone 1947/1947 and 7/7, no reruns. macOS not run.
- 2026-11-18: 0027 stage 7 (cleanup), 988c60f..04b842c, every change test-first. A String byte written through a
  subscript passed the checker and failed in the backend (older than the branch); it was the last producer of
  `subscript_addr`. Spec 2.1.1 now makes a byte read-only whatever the binding: `&s[i] = b` and `op=` are
  `assignment-to-immutable`, an `inout` `&s[i]` is `invalid-inout-argument`, `consume s[i]` is
  `invalid-consume-argument` (one report, also for a non-consumable base), each with help (tests 3e05aa3, 5c00858,
  054396f observed failing; fixes 31c4b46, 5ca4da4, 04b842c; 2382569 corrects a test of mine that passed a byte to a
  consuming parameter without `consume`). Then the cleanup (tests 439702d and e79cd36 observed failing; 526bdc7):
  `TypeKind::dictionary`, `construct_dictionary`, `dictionary_set`, `dictionary_get` and `subscript_addr` are deleted
  with their checker (builtin `get` and its signature), lowering, verifier and backend cases (and the backend's
  key-kind table); slots 39-41, 50 and type kind 11 stay rejected, `subscript` and `count` are String-only and pinned
  by invalid-IR tests, the wire is 25. The runtime dictionary ABI stays: the library calls it. The hand-built tests of
  the builtin operations went with them; the one role rule they alone covered (one declaration per optional role)
  moved to `VerifierRejectsMalformedRoleEnumerations`. Reviews: type-system (no blocking; low findings fixed, one
  re-review round) and IR/backend (no blocking; `consume s[i]` reaching lowering fixed, test gaps filled). The cost
  gate was not re-measured: every deleted path is unreachable from a library `Dict` program, and the checker change
  touches only String byte writes. Validation at 04b842c: Windows AMD64 Debug and Release `ctest` 1976/1976 and
  `fern test` 7/7 (Debug: Compression.Interoperability, Release: ValidationScriptTests, Fern.Manifest,
  Fern.Workspace and Compression.Interoperability passed only on rerun, while two other sessions ran their own
  validations on the machine); Linux x86_64 fresh WSL clone 1952/1952 and 7/7 in each, no reruns. macOS not run.
- 2026-11-18: Merged `main` (#29, the verifier's dominator tree; #30, `op=` index operands analyzed once) as 2a27377.
  Conflicts: `analyzeAssignment` keeps #30's order for `op=` and this branch's order for `=`; the literal protocols
  use `DominanceAnalysis`; both sides' fixtures stay. Validation at 2a27377: Windows AMD64 Debug and Release `ctest`
  1999/1999 and `fern test` 7/7 (ValidationScriptTests, Fern.Manifest, Fern.Workspace passed only on rerun, with
  the reviewers and the Linux run on the machine); Linux x86_64 fresh WSL clone 1975/1975 and 7/7, no reruns.
  Type-system and IR/backend reviews of the merge: nothing blocking. Their findings, each fixed test-first:
  `=` blamed its right side for a target consumed before the statement and underlined the `&` (test d9db4a9 observed
  failing; fix 9fef5b6 passes the blame as `RightSide`, as #30 does for `op=`); and, older than the merge, the
  dictionary literal protocol's rule 4 walked the function per stored value, so a Debug compile of a 1600-entry
  literal amid 3200 `if` blocks took 77 s and an 800-entry `?` literal in a loop before 6400 `if`s 42.7 s. A step
  counter (ad8a830, `VerificationResult::literalPathWork`) and a bound test (2b3ceff, e316b86 observed failing:
  101040, 65581 and 69908 steps against 8 per block) came first; 05d8bdd walks once per literal, and 34cb81b answers
  the walks that avoid a defining block with a `DominanceAnalysis` rooted at the packing block, in each direction,
  checked against avoiding walks on 2000 random graphs (30caf1d, which also sets the bound to 16 per block after
  seeing about 12). Those programs now take 2.3 s and 4.3 s (the array controls 1.8 s and 4.0 s). Also: a
  durable `=` rejection fixture, a pinned unreachable `op=`, cross-block dominance and self-loop cases for both
  literal protocols (d552c5c, d6602c9; each fails with its check disabled), and IR notes (f5a97e2, 7d518da).
  Re-reviews: both clean. Recorded, not fixed: a literal whose values need the walks still costs O(blocks), so many
  such literals in one function cost literals x blocks; the array literal protocol has no use-once rule (lowering
  never stores a value twice); a rejected `op=` on a dictionary element skips type checking its right side and
  leaks an internal deferred-reference error, which is on `main` since #10 and was proposed as its own task.
  Validation at a14b5f2: Windows AMD64 Debug and Release `ctest` 2005/2005 and `fern test` 7/7 (Debug:
  Compression.Interoperability and E2EOptimizationEquivalence_integer_conversions timed out once and passed on
  rerun, the latter in 1.2 s, a program with no literals; Release: Fern.Workspace); Linux x86_64 fresh WSL clone
  1981/1981 and 7/7 in each, no reruns. macOS not run.

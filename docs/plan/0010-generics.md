# 0010: Generics

- Status: in progress (updated 2026-11-13)
- Decisions: [0020](../decisions/0020-function-values.md) (Phase 1),
  [0021](../decisions/0021-protocols-and-generic-functions.md) (Phase 2). Background:
  [0009](../decisions/0009-runtime-core-capabilities.md), [0015](../decisions/0015-noncopyable-and-linear-types.md),
  [0018](../decisions/0018-tasks.md), [0019](../decisions/0019-channels.md)
- Spec: [§2.3](../../specs/02-types.md), [§2.6](../../specs/02-types.md), [§3.2.3](../../specs/03-declarations.md),
  [§20](../../specs/20-protocols.md), [§21](../../specs/21-concurrency.md)

## Goal

Add function values, generic code and type-layout knowledge to the language, so that the compiler's
hard-coded generic builtins become ordinary Joyeer library types. Today the compiler supplies type
knowledge the runtime cannot express (element layouts, destructors, the task entry thunk, `Result`
construction) through dedicated IR opcodes (`taskSpawn`, `taskJoin`, `channelCreate`, `channelSend`,
`channelReceive`), although the algorithms are already Joyeer in [lib/Runtime/core/](../../lib/Runtime/core/).

## Definition of done

- A user can pass a top-level function as a value, write generic functions bounded by protocols, and
  declare generic structs and enums.
- `JoinHandle<R>`, `ChannelSender<T>` and `ChannelReceiver<T>` are declared in Joyeer over the existing
  `joyeer_thread_*` and `joyeer_channel_*` runtime functions, with the same public API and diagnostics as
  §21.
- The task and channel opcodes, `TypeKind::task`, `channelSender` and `channelReceiver`, and their
  backend cases in [native_collections.cpp](../../compiler/backend/native_collections.cpp) are removed.
- Tasks and channels are no slower than today, measured in both optimization profiles.

## Current state

| Feature | State |
|---|---|
| Function types `(T): R` | Implemented in Phase 1 ([0020](../decisions/0020-function-values.md)); non-capturing top-level function values, with exact access effects. |
| Indirect calls | Typed function-value calls are implemented; the runtime-only untyped `joyeer_intrinsic_call_void1/2` also remain (decision 0009, D2). |
| Protocols and generic functions | §20 is normative for v1; Phase 2 implementation and acceptance are tracked below. `trait` remains reserved. |
| Generic structs, enums, methods | Deferred by §20.10; v0.1 forbids them (§2.6, §3.2.3). |
| Layout intrinsics | None; runtime code sees only `Int` addresses ([decision 0005](../decisions/0005-runtime-unsafe-memory.md)). |

## Todo

Each phase follows the [feature workflow](../decisions/README.md): decision record, spec, failing
tests, implementation, role reviewers run separately from the author, then
`python3 scripts/validate.py --llvm-root <sdk>` in Debug and Release.

### Phase 1: function values

Non-capturing references to top-level functions; access effects are part of the type and must match
exactly. No closures, methods or generic functions as values.

- [x] Decision record: function values without captures, and why captures stay out. Evidence:
  [0020](../decisions/0020-function-values.md), accepted 2026-11-12 after language-designer and
  ir-backend-reviewer reviews (8801ec1, 24fca8f).
- [x] Spec: make §2.3 function types normative; mark what is not accepted. Evidence: §2.3.1 and §5.6.1
  with the amended sections listed in 0020 (24fca8f).
- [x] GoogleTests and stage fixtures for each access effect and each rejected form. Evidence: parser,
  type-checking, semantic-analysis, IR lowering/verifier, LLVM and wire tests, two CLI diagnostic
  fixtures (c292e32). They were written alongside the implementation, not strictly first; five of them
  failed and found real bugs before passing.
- [x] Name resolution and type checking: a function `TypeKind`, a function name used as a value.
  Evidence: c292e32.
- [x] IR: a function-address opcode and an indirect-call opcode; verifier checks signature and ownership.
  Evidence: c292e32, `NewOpcodeVerifierTest.FunctionAddressAndIndirectCallsAreCheckedAgainstTheFunctionType`.
- [x] Backend: function pointers, indirect calls with the Joyeer calling convention, debug info.
  Evidence: c292e32, `LLVMBackendTest.CallsThroughFunctionValuesAreOneIndirectCall`; the e2e fixture
  builds and runs identically with `-gfull`/`-gdwarf` at -O0, -O2 and -O3.
- [x] End-to-end fixture: store, pass and call function values, including `consuming` and `inout`.
  Evidence: `tests/e2e/function_values.joyeer`, `E2EFunctionValuesO0`..`O3` and
  `E2EOptimizationEquivalence_function_values`; Windows x64 Debug ctest 1527 of 1527.
- [x] Close the "function used as a value" diagnostic gap in the [concurrency plan](0009-concurrency.md).
  Evidence: a function used as a value is now a typed value or `not-a-function-value` with its reason
  (`TypeCheckingTest.RejectsFunctionsThatCannotBeValues`, `MembersThatMustBeCalledAreNotFunctionValues`).
  The `spawn(...).join()` half of that gap is unrelated and stays in plan 0009.
- [x] Reviews (language-designer, type-system-reviewer, ir-backend-reviewer, adversarial-tester).
  Evidence: findings recorded in decision 0020; all fixed with regression tests, and the adversarial
  probes re-run clean.
- [ ] Validation: `scripts/validate.py` (Debug and Release) and Linux. Linux x86_64 (Ubuntu on WSL, LLVM 22.1.8)
  on the exact Phase 1 tree `945ced9`: Debug and Release build and ctest pass, 1511 of 1511 each. `fern test` is
  6 of 7 in both: `SystemTests` fails its "stderr closed descriptor" check (exit status 1), which also failed
  6 of 7 in an earlier Linux report on `82c493c`, before Phase 1, so it is not caused by function values.
  Still open: `validate.py` on Windows (Python is installed now, not yet on the shell's PATH) and macOS;
  Windows x64 Debug ctest was run by hand instead.
- [x] Update [supported features](../impl/supported-features.md). Evidence: the "Function values" row.

### Found along the way (not function values)

- [ ] The compiler overflows its stack on deep nesting (about 200 parenthesized expressions, 400
  parenthesized types, a 300-term `+` chain); add a depth limit with a diagnostic. Pre-existing.
- [ ] `struct S { var n: Never }` is accepted; reject `Never` as a stored field type. Pre-existing.

### Phase 2: protocols and generic functions (§20 v1)

- [x] Decision record: reviewability of monomorphized generics, ownership of a type parameter, and
  where instantiation happens. Evidence: [0021](../decisions/0021-protocols-and-generic-functions.md),
  accepted 2026-11-12 after a language-designer review (8d9d643).
- [x] Spec: settle §20 v1 and update §2.6, §3.2.3 and the reserved-keyword appendix. Evidence: §20
  made normative (inference §20.4.1, entailment §20.4.2, expanding recursion §20.6), with §1.4, §2.6,
  §2.9, §3.2.3, §3.5, §4.1.1, §5.4, §7, §12.2, §12.4, §21.2.1 and Appendix C amended as listed in 0021.
- [x] Failing GoogleTests and stage fixtures for protocols, conformances, constraints and `Copyable`.
  Evidence: parser/name-resolution/type-checking fixtures, CLI rejection/report fixtures, and focused
  tests in the existing stage suites. The initial implementation preceded these tests; failing
  regressions reproduced all nine distinct implementation-review findings before their fixes.
- [x] Parsing and name resolution: `protocol`, conformance extensions, generic parameters.
  Evidence: `ParsesProtocolsConformancesAndGenericConstraints`,
  `ResolvesGenericParameterAndProtocolSelfInTheirOwnScopes`, and the public-signature/module fixtures.
- [x] Type checking: check a generic body once against its constraints; call-site errors name the
  unmet constraint. Evidence: generic inference, conformance, ownership, visibility and unused-body
  regressions, plus `DiagnosticsCliRejectsUnsatisfiedGenericConstraint`.
- [x] Instantiation with a cache keyed by function and type arguments; spans and debug scopes point at
  the generic source. Evidence: transitive/cache and source-scope tests, bounded recursion/depth tests,
  private-name collision coverage and the byte-exact `GenericsInstantiationReport` at `-O0`/`-O2`.
- [x] IR, verifier and backend see only concrete functions; end-to-end fixtures. Evidence:
  `GenericInstanceNamesAndConcreteDebugTypesSurviveTheBackendWire`, literal/used/nested placeholder
  rejection, `E2EGenericsO0`..`O3`, `E2EGenericsPropagationO0`..`O3` and `E2EGenericDependencyO0`/`O2`.
- [x] Measure compile time and code size on fern and the web-server example. Evidence:
  [local sample](../impl/backend.md#phase-2-generics-completion-sample), against `3feb624`; code/file
  sizes unchanged and compile/link medians effectively unchanged to 1.4% higher in this sample.
- [x] Type-system and IR/backend implementation reviews, separately from the author. Evidence:
  nine distinct findings reproduced and fixed with passing regressions, recorded in 0021.
- [x] Independent adversarial review. Evidence: adversarial-tester found five minor defects and no crash or
  miscompile; the `<error>` cascade and generic `main` are fixed with regression tests
  (`AConflictingTypeArgumentDoesNotCascadeIntoAFunctionTypeMismatch`, `TheProgramEntryPointCannotBeGeneric`).
  Three are recorded as open in 0021: a literal does not take an already-inferred `T?`, user structs may
  reuse reserved protocol names, and a method named `Self` is accepted.
- [ ] Full validation in Debug and Release, and Linux. blocked: Python is absent, so `validate.py`
  cannot run; manual Debug root `fern test` also reproduces the baseline `SystemTests` stderr
  descriptor failure (0021). Windows x64 Debug default build and unfiltered CTest pass 1591/1591.
  Release-host/Linux and the debugger breakpoint check remain unrun.
- [x] Update supported features and implementation notes, correct the spec example, and record
  implementation deviations in 0021. Evidence: the linked source-of-truth documents in this change.

### Phase 3: generic types and layout intrinsics

- [x] Decision record: generic structs and enums (lifting part of §20.10), and the runtime-only layout
  intrinsics' surface. Evidence: [0022](../decisions/0022-generic-types-and-layout-intrinsics.md), accepted
  2026-11-13 after a language-designer review.
- [x] Spec for generic structs, enums and their methods. Evidence: new §20.12 (construction inference,
  extensions and unconditional conformance, ownership, termination, layout intrinsics) with §2.6, §3.2.3,
  §3.3, §3.5, §3.9, §4.1.1, §5.6.1, §19.4.5, §20.4, §20.4.1, §20.4.2, §20.8, §20.10 and Appendix C amended
  per 0022. Documentation only; not built or run.
- [x] Failing tests, then parsing, type checking and instantiation of generic types. Evidence: parser, name-resolution and type-checking unit tests, `E2EGenericTypes`/`E2EGenericDeinit` at -O0 to -O3, `GenericTypesInstantiationReport`, `TypeGenericTypesRejectExpandingRecursion`; expected-type inference, type-level expanding recursion, the exposed-constraint check and report lines are included. Windows x64 Debug, full ctest 1628 of 1628.
- [x] Runtime-only intrinsics: size and alignment of `T`, moving a `T` into and out of an address, and
  destroying a `T` at an address. Evidence: `tests/c-interop/layout-intrinsics.joyeer`
  (`JoyeerCInterop.LayoutIntrinsicsDebug`/`Optimized`) and four unit tests in `cinterop_test.cpp`; Windows x64
  Debug only.
- [x] Fixture: a runtime-source generic struct owns a raw buffer of `T` and destroys queued noncopyable
  values correctly. Evidence: `tests/c-interop/generic-queue.joyeer` (`JoyeerCInterop.GenericQueueDebug`/
  `Optimized`), including the runtime's allocation-balance check at exit.
- [ ] Reviews and validation; update supported features.

### Phase 4: concurrency builtins in the library

- [ ] Benchmark today's tasks and channels as the baseline.
- [ ] Declare `JoinHandle<R>`, `ChannelSender<T>`, `ChannelReceiver<T>`, `spawn` and `channel` in Joyeer,
  keeping §21: linear handles, `Result` from `send` and `receive`, destruction of unsent and unreceived
  values, the 256-task limit and its traps.
- [ ] Existing §21 fixtures and diagnostics pass unchanged.
- [ ] Benchmark again; no regression in either profile.
- [ ] Delete the opcodes, type kinds, backend cases and compiler-known names.
- [ ] Reviews and validation; update supported features, runtime notes and §21.
- [ ] Write a follow-up plan for `Optional` and `Result` as library enums, and later `Array` and `Dict`.

## Open questions

Questions 1 to 3 are answered in [0021](../decisions/0021-protocols-and-generic-functions.md) (accepted).

1. **Reviewability.** The preamble wants code that humans review; monomorphization compiles one body many
   times. How do bounded, statically dispatched generics keep behavior predictable, and what does the
   compiler report (instantiations, size) to keep it visible? Phase 2 record.
2. **Ownership of a type parameter.** `Copyable` covers copying; how does a generic body treat a `T` that
   may be noncopyable or `linear` (decision 0015)? Phase 2 record.
3. **Where instantiation happens:** on the typed model after type checking, or during IR lowering.
   Phase 2 record.
4. **Captures.** What would justify closures later, given ownership of captured state? Phase 1 record.
5. **Linear values in generic types.** 0021 bans linear type arguments of generic functions, but a
   library `Array<T>` must hold `JoinHandle<R>`. How does generic code end a linear value? Phase 3
   record.

## Out of scope

Closures and captures, existential values (`any P`) and dynamic dispatch, associated types, default
protocol methods, protocol inheritance, explicit type arguments at call sites, and variadic generics.
They stay deferred per §20.10 unless a phase's record shows they are needed.

## Progress log

- 2026-11-14: Phase 3 validation on Linux x86_64 (WSL Ubuntu, `validate.py`, tree 2317f09): Debug and Release
  build, ctest 1621 of 1621 each; `fern test` 6 of 7 in both. The failure is the SystemTests stderr check
  `UNEXPECTED stderr closed descriptor (0 bytes): ok` (a zero-byte write to a closed descriptor reports `ok`,
  the test expects `err:9`). The same check fails the same way on `main` under WSL, so it is not caused by
  this phase; it needs its own fix or an expected-platform note. Windows x64 Debug ctest 1644 of 1645
  (`ValidationScriptTests` timeout under load, passes alone). Windows `validate.py` and macOS are not run, so
  the "Reviews and validation" item stays open.
- 2026-11-14: Phase 3 role reviews (type-system, IR/backend, adversarial). Fixed with regression tests: a
  compiler crash (stack overflow) on `struct S<T> { var next: S<[T]>? }` once a value is copied (the type-cycle
  check now runs before any body); spurious `conflicting-type-argument` for `Box<Int?> = Box(value: 1)`;
  type-argument checks now apply to annotations inside bodies; `let c: Never` and `Box<Never>` locals no longer
  crash; payloadless cases of generic enums (`Tree.Leaf`, `[.Leaf]`) take their type from the expected type;
  verifier rules for the layout opcodes (concrete queried type, not `Never`, runtime functions only);
  `generic-queue` sizes its buffers with `size_of` (a struct with a `deinit` has a hidden live flag, which an
  -O1 run exposed). Still open, not fixed: deep nesting overflows the stack with no diagnostic (also
  pre-existing, see "Found along the way"); expanding recursion through witnesses, struct construction and
  deinit ends at the instantiation limit; `internInstanceTypes` is quadratic; `spawn` of a generic function;
  diagnostic IDs `invalid-type-argument-count` and `type-mismatch` differ from the names in §20.8/0022;
  `size_of<Int>()` in an unauthorized unit reports a comparison error; `>=` is not split after a type argument;
  `Any` is not expressible yet, so its rejection as a type argument is untested.

- 2026-11-13: Phase 3 layout intrinsics and expected-type inference. `name<T>(...)` parses for the five
  `joyeer_intrinsic_*` layout names only in a unit selected with `--runtime-module` (`Parser` flag); they are
  prelude builtins that the type checker rejects elsewhere with `unauthorized-intrinsic`. New IR opcodes
  `typeSize`, `typeAlignment` (constant expressions the target layout folds, since the portable module has no
  layout) and `integerAddress`; `move_in`/`move_out`/`destroy_at` lower to store, take and destroy on it. Wire
  format 17. A construction's expected type now fills open type parameters (spec 20.12.1). Fixtures
  `layout-intrinsics` and `generic-queue` pass at -O0 and -O2. Windows x64 Debug full ctest 1627 of 1627. Not done: expanding
  recursion through types, instantiation report lines for types/methods, the exposed-constraint check, role
  reviews, Release/Linux and `validate.py` (no Python here), supported-features update.

- 2026-11-13: Phase 3 generic `deinit`: the `deinit` of a generic struct is an instance per concrete instance of the
  struct (found to a fixpoint with the instance types) and is linked to that instance's IR definition. New fixture
  `tests/e2e/generic_deinit.joyeer` (`E2EGenericDeinitO0`..`O3`): each instance runs its own `deinit` once, in
  reverse declaration order. Windows x64 Debug full ctest 1614 of 1614. Not done: the layout intrinsics, expected-type
  inference (B2), expanding recursion through types, instantiation report lines for types/methods, the Queue fixture.
- 2026-11-13: Phase 3 instantiation and lowering of generic structs and enums: concrete instance layouts are
  interned (to a fixpoint) and lowered per type, methods of generic types are instances keyed by the receiver's
  arguments (including witnesses reached through a requirement call), enum cases substitute the instance's
  arguments, and the IR verifier allows a field symbol to repeat across instances. New e2e fixture
  `tests/e2e/generic_types.joyeer` (`E2EGenericTypesO0`..`O3`) passes; full Windows x64 Debug ctest
  (before the fixture was registered) passed 1605 of 1605 after the lowering and verifier edits. Not done: `deinit` of a generic
  struct, the intrinsics, expected-type inference (B2), expanding recursion through types, `--report-instantiations`
  for types and methods, Self-in-extension edge cases, and the Queue fixture.
- 2026-11-13: Phase 3 type checking, second slice: explicit type arguments of generic types are checked against
  their constraints, `Never` and linear (deferred until constraints are known); `invalid-extension-header`;
  conformance of generic types (`extension Box<U>: P`) is recorded against the declaration's instance, matches
  every instance, and satisfies constraints inside generic bodies. CompilerTypeCheckingTests 206 of 206.
  Not done: expected-type inference (B2), expanding recursion through types, intrinsics, and all of
  instantiation of types and methods, IR and backend. A duplicate extension-parameter name is reported by
  name resolution, not the type checker.
- 2026-11-13: Phase 3 type checking, first slice: `TypeContext` interns generic structs/enums by arguments,
  `Self` and extension parameters resolve per context, construction of a generic struct or enum case infers
  its arguments through the existing generic-call inference, members and methods substitute the receiver's
  arguments, `Box<Int>` displays with its arguments, and copyability/linearity are computed per instance.
  Four new TypeCheckingTests pass; CompilerTypeCheckingTests 203 of 203 (Windows x64 Debug). Not done:
  constraint/`Never`/linear checks on explicit type arguments, expected-type inference (B2),
  `invalid-extension-header`, conformance of generic types, expanding recursion, intrinsics, and all of
  instantiation, IR and backend.
- 2026-11-13: Phase 3 name resolution: struct/enum type parameters are declared in the type's member scope
  (`Symbol::typeParameters`), methods see them through their function scope, and an `extension Box<U>` header
  declares fresh local parameters (`SemanticModel::extensionTypeParameters`) sharing the declaration's
  constraint scopes. Tests `GenericStructsEnumsAndExtensionsBindTheirTypeParameters` and
  `GenericTypeParametersDoNotEscapeTheirDeclaration`; CompilerNameResolutionTests 67 of 67 (Windows x64 Debug).
  Not done: exposed-constraint check (§12.2) for struct/enum parameters; everything from type checking on.
  Windows x64 Debug full build and unfiltered ctest: 1596 of 1597 pass; the one failure,
  `JoyeerCInterop.MissingLibrary`, is a nested build that cannot find `kernel32.lib` in this shell (LNK1104),
  not a result of these changes, but it was not re-run in a clean Developer shell.
- 2026-11-13: Phase 3 parser stage: `struct` and `enum` accept `<T: P & Q>` (syntax `genericParameters`);
  tests `ParsesGenericStructsEnumsAndExtensionHeaders`, `RejectsMalformedGenericTypeParameterClauses` and
  fixture `tests/parser/ok/generic_types.joyeer` pass (CompilerParserTests, Windows x64 Debug). The parser
  change was written before its tests, not after a failing run. Not done: intrinsic call-site type arguments,
  name resolution, type checking and everything after. No other plan item checked.
- 2026-11-13: 0022 accepted; spec item checked (§20.12 and the amendments listed in 0022). Next: failing
  tests, then parser, name resolution, type checking, instantiation, IR/backend, intrinsics, fixture.
- 2026-11-13: Phase 3 decision record 0022 drafted and reviewed by language-designer (5 blockers, 7 should-fix
  items, all folded in) and accepted. Decision item checked; no spec,
  tests or code exist for Phase 3.
- 2026-11-12: adversarial review done and two of five findings fixed; Debug ctest 1593 of 1593. Only
  full validation (Release, Linux, `validate.py`) remains, blocked on Python.
- 2026-11-12: Phase 2 implementation work completed: reporting, stage/CLI/module/e2e coverage,
  nine distinct implementation-review findings fixed, and follow-up inference, diagnostic and
  naming regressions covered. Windows x64 Debug default build and unfiltered CTest pass 1591/1591.
  Final fern/web-server timing and footprint samples recorded; code/file sizes unchanged and both
  reports have zero user generic instances. Acceptance remains open: Python is absent,
  root `SystemTests` repeats its baseline stderr failure, the adversarial review was rejected by the
  service before execution, and Release-host/Linux/debugger checks are unrun. Docs and portable
  skill references updated; only verified items checked.
- 2026-11-12: Phase 2 implementation in progress, not reviewed: parser (`protocol`, conformance lists,
  `<T: P & Q>`), name resolution, type checking (inference, constraints, conformances, expanding
  recursion, instantiation) and lowering of instances are written; `tests/e2e/generics.joyeer` builds and
  prints the expected output by hand. Not done: GoogleTests and stage fixtures, the
  `--report-instantiations` flag, the full ctest run, the role reviews, the docs. No items checked.
- 2026-11-12: spec item checked (§20 normative for v1).
- 2026-11-12: decision 0021 accepted; the decision-record item checked.
- 2026-11-12: Phase 2 decision record 0021 drafted (open questions 1 to 3) and reviewed by
  language-designer (3 blockers and 14 should-fix items, all folded in); awaiting acceptance. No items
  done.
- 2026-11-12: implementation review fixes (use-after-free through nested value calls and seven more);
  three items checked (gap, reviews, supported features); validation blocked on Python; two pre-existing
  bugs added under "Found along the way".
- 2026-11-12: Phase 1 implemented (c292e32): seven items checked. Windows x64 Debug ctest passes 1527
  of 1527. Role reviews (type-system, ir-backend, adversarial-tester) are running; `validate.py`,
  Release and other platforms not run yet.
- 2026-11-12: decision record 0020 drafted and reviewed by language-designer and ir-backend-reviewer;
  awaiting acceptance. No items done.
- 2026-11-12: plan written; no items done.

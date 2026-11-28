# 0005: Test Coverage

- Status: in progress (updated 2026-11-28)
- Decisions: none
- Spec: all chapters; coverage is measured against them and against
  [supported features](../impl/supported-features.md)

## Goal

Complement hand-written tests with systematic and adversarial coverage: fuzzing, sanitizers, CI,
generated ownership programs, and traceability from the spec to the tests.

## Definition of done

Every todo item below is done or explicitly dropped, and spec coverage can be measured by a script.

## Current state

Coverage comes from [GoogleTests](../../tests/unittests/), stage-specific
[fixtures](../../tests/), and [native end-to-end fixtures](../../tests/e2e/).
On normal program exit, the [runtime entry point](../../lib/Runtime/core/entry.joyeer) fails
if its allocation counter is nonzero. This checks allocation balance for the cases that run;
it does not replace sanitizer instrumentation or generated ownership cases.

## Todo

### Completed

- [x] Differential testing across optimization levels. Evidence: the
  [CTest registration](../../tests/unittests/compiler/CMakeLists.txt) selects self-contained
  native fixtures for `E2EOptimizationEquivalence_<fixture>`. The
  [comparison script](../../scripts/cmake/verifyOptimizationEquivalence.cmake) runs each at
  `-O0` through `-O3` and compares exit status, stdout, and stderr.
- [x] Remove the stale legacy fixtures. Evidence: `tests/basis` and `tests/errors` are gone.
- [x] Confirm the Clang-gated native entry fixtures run on macOS ARM64. At `5c21924e`, all
  15 `E2EEntry*` cases passed in both full configurations, including POSIX raw bytes,
  invalid, duplicate, and missing entry rejection, and exit status underflow and overflow.
  Separate configure probes verified that Intel and universal builds are rejected.

### Remaining

In priority order.

- [x] Opt-in host ASan/UBSan (MSVC AddressSanitizer on Windows). The
  lexer/parser checks passed on Windows ARM64 and WSL2 Linux AArch64.
- [x] Instrument the Joyeer-emitted runtime core in a separate Linux test
  archive. On WSL2 Linux AArch64, `RuntimeAddressSanitizer` accepted a valid
  read and reported the intended one-byte read beyond an allocation in Joyeer
  code at `-O0` and `-O2`. Ordinary executable emission is unaffected by
  the old environment trigger. See [sanitizer builds](../building.md#full-platform-validation).
- [ ] Tighten the test-only ASan object check to reject non-core dependency
  definitions, not just non-core root definitions. Cover a mixed-module input
  at `-O0` and `-O2` without changing ordinary object emission.
- [ ] Broader generated runtime sanitizer coverage: the Linux test archive
  covers the core's instrumented accesses, not logical bounds within its
  custom allocator, other generated programs, or Windows runtime objects.
- [x] Bounded byte-mutation fuzz regression at the lexer/parser boundary:
  `ParserTest.MutatedBytesProduceDiagnosticsOrABoundedSyntaxTree` feeds byte insertions,
  substitutions, deletions and truncations through both stages under a CTest timeout.
  It passed on Linux AArch64 (WSL2). Type-checker fuzzing is a separate step.
- [x] Generated ownership matrix: `GeneratedOwnership` exercises moves, loops, branches, early
  returns, and `?` at `-O0` and `-O2`; it checks output and the runtime allocation balance.
  The runner passed on Windows ARM64 and Linux AArch64 (WSL2).
- [x] Spec-to-test traceability: [citation convention](../building.md#full-platform-validation)
  and [`scripts/spec-coverage.py`](../../scripts/spec-coverage.py) list uncited sections and
  reject invalid citations. Verified on Windows and Linux.
- [ ] CI: run [`scripts/validate.py`](../../scripts/validate.py) per change or on a schedule.
  The [weekly workflow](../building.md#native-validation-workflow) is checked in.
  blocked: the repository Actions runner inventory contains no self-hosted runners;
  there is no hosted execution evidence.
- [x] Audit negative coverage for the lexer, module resolution, and native entry.
  `LexerTest` covers literal overflow, malformed separators, and reserved words;
  `DiagnosticsCliRendersLexerSourceContext` pins the unterminated-comment diagnostic.
  `ModuleServiceTest` covers dependency cycles and private/internal visibility across
  modules. `E2EEntry*` covers invalid, duplicate, and missing entry, and exit limits.
  These tests ran in the ARM64 Debug and Release validators.
- [ ] Pin today's diagnostic in one negative fixture per remaining specified but
  [unimplemented feature](../impl/supported-features.md#not-implemented), such as
  declaration-level contract syntax, to catch silent acceptance. Added 24 stage
  fixtures for deferred contracts, imports, declarations, generics, patterns, and
  operators, plus an individual lexer assertion for each reserved word.
  `DiagnosticsRejectsDeferred.*`, `LexerTest.ReportsEachReservedWordAsUnsupportedSyntax`,
  and the deferred annotation cases pass on Windows ARM64 and WSL2 Linux AArch64.
  The remaining inventory includes native-memory proposals and features without
  a selected source spelling; do not invent a fixture spelling for those.
- [ ] Audit remaining platform-specific negative tests. The
  [entry fixtures](../../tests/unittests/compiler/CMakeLists.txt) have been verified on
  macOS ARM64 as above; all 15 `E2EEntry*` tests and `Fern.HostFailures` also passed on
  Linux AArch64 (WSL2), including raw byte arguments and the executable-entry diagnostics.
  Confirm the Clang-gated entry cases in the
  [Linux x86-64 runs](../impl/runtime.md#collection-validation) and identify any remaining
  POSIX negative-coverage gaps.
  [Surrogate rejection](../../tests/unittests/runtime/entry_encoding_test.cpp) and `/MT` are
  Windows-specific.

## Open questions

None.

## Out of scope

Running the existing suites; that is the job of `scripts/validate.py` and the platform plans.

## Progress log

- 2026-11-28: Added explicitly selected, test-only runtime ASan emission
  and verified a real report from Joyeer code on WSL AArch64. Fixed the
  compiler driver's allocation leaks found when building the probe, and
  verified ordinary output does not inherit test instrumentation.
- 2026-11-28: Pinned the current diagnostics for 24 deferred syntax cases and the
  reserved-word list. Made the stdin optimization comparison reopen the same
  fixture input for every run instead of consuming the test runner's input.
- 2026-11-28: Added opt-in host ASan/UBSan configuration and the native validation
  workflow. Generated runtime instrumentation and hosted runner provisioning remain open.
- 2026-11-28: Added byte mutation, generated ownership, the spec citation report,
  and stable diagnostics for the deferred annotations. Audited lexer and module negatives,
  Linux AArch64 entry and fern host failures; the separate x86-64 case evidence is still open.
- 2026-11-26: Verified the macOS ARM64 native entry cases and architecture rejection.
  The remaining platform and POSIX negative-coverage audit stays open.
- 2026-11-23: separated the two completed items from the remaining work and replaced the old
  fixture counts with links to test registration and existing platform evidence. No new tests
  were run.
- 2026-11-12: converted to the plan template; the optimization-equivalence and legacy-fixture items were
  already done and are checked.

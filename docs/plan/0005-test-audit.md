# 0005: Test Coverage

- Status: in progress (updated 2026-11-26)
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

- [ ] Sanitizer builds: a CMake option for ASan/UBSan (MSVC AddressSanitizer on Windows) for the compiler
  and runtime, plus one validation row. The [runtime plan](0008-runtime-in-joyeer.md) leans on this, because
  the runtime's address code is unchecked.
- [ ] A byte-mutation fuzzer for the lexer and parser that checks only "no crash, no hang, a diagnostic
  or a valid AST"; the type checker later.
- [ ] A generator of ownership programs combining moves, loops, branches, early returns and `?`
  propagation, run with the allocation-balance check. Today ownership relies on hand-written cases
  (`use_after_consume`, `consuming`, `projection_consume`, `regression_flow_*`).
- [ ] Spec-to-test traceability: a `// spec: 04-memory.md §4.2` convention in new tests and a script
  that lists spec sections with no citations.
- [ ] CI: run [`scripts/validate.py`](../../scripts/validate.py) per change or on a schedule.
  The local validator exists; no workflow is checked in.
- [ ] Expand negative fixtures in sparse stages, especially the lexer and native entry. Name
  resolution now has registered regressions for generic scope and file-owner visibility. Audit
  remaining visibility and cycle checks ([§12](../../specs/12-modules.md)), literal range and
  separator rules ([§1](../../specs/01-lexical.md)), and reserved words
  ([appendix A](../../specs/appendix-a-reserved.md)).
- [ ] Pin today's diagnostic in one negative fixture per remaining specified but
  [unimplemented feature](../impl/supported-features.md#not-implemented), such as
  declaration-level contract syntax, to catch silent acceptance.
- [ ] Audit remaining platform-specific negative tests. The
  [entry fixtures](../../tests/unittests/compiler/CMakeLists.txt) have been verified on
  macOS ARM64 as above. Confirm the Clang-gated entry cases in the
  [Linux x86-64 runs](../impl/runtime.md#collection-validation) and identify any remaining
  POSIX negative-coverage gaps.
  [Surrogate rejection](../../tests/unittests/runtime/entry_encoding_test.cpp) and `/MT` are
  Windows-specific.

## Open questions

None.

## Out of scope

Running the existing suites; that is the job of `scripts/validate.py` and the platform plans.

## Progress log

- 2026-11-26: Verified the macOS ARM64 native entry cases and architecture rejection.
  The remaining platform and POSIX negative-coverage audit stays open.
- 2026-11-23: separated the two completed items from the remaining work and replaced the old
  fixture counts with links to test registration and existing platform evidence. No new tests
  were run.
- 2026-11-12: converted to the plan template; the optimization-equivalence and legacy-fixture items were
  already done and are checked.

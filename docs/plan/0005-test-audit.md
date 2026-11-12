# 0005: Test Coverage

- Status: in progress (updated 2026-11-12)
- Decisions: none
- Spec: all chapters; coverage is measured against them and against
  [supported features](../impl/supported-features.md)

## Goal

Move testing from hand-written cases to systematic and adversarial coverage: fuzzing, sanitizers, CI,
generated ownership programs, and traceability from the spec to the tests.

## Definition of done

Every todo item below is done or explicitly dropped, and spec coverage can be measured by a script.

## Current state

A snapshot audit; counts come from `TEST(...)` macros and fixture file listings, and none of the tests
were run for it.

| Layer | Evidence |
|---|---|
| GoogleTests, compiler | about 460 cases: semantic analysis 78, IR lowering 61, parser 56, LLVM 50, type checking 40, lexer 38, C interop 35, IR 29, module 28, name resolution 26 |
| GoogleTests, runtime | runtime, host and argument tests; the host operations are tested through `SystemTests` |
| End-to-end fixtures | 79 files in `tests/e2e`, mostly checked-arithmetic and bounds traps, ownership, and `regression_*` programs |
| Negative fixtures | semantic-analysis 13, type-checking 17, parser 6, native 3, lexer 1, name-resolution 1 |
| Leak detection | `lib/Runtime/core/entry.joyeer` reads `joyeer_runtime_active_allocations()` at exit, so end-to-end fixtures check allocation balance |
| Optimization | one `-O2` script (`scripts/cmake/verifyOptimizedNativeFailure.cmake`), two fixtures (`optimized_bounds`, `optimized_overflow`), and the equivalence run below |

## Todo

In priority order.

- [x] Differential testing across optimization levels. Evidence: `E2EOptimizationEquivalence_<fixture>`
  runs 62 self-contained `tests/e2e` fixtures at `-O0` to `-O3` and requires identical exit status,
  stdout and stderr ([script](../../scripts/cmake/verifyOptimizationEquivalence.cmake)). Fixtures
  needing arguments, a working directory or input files are excluded.
- [x] Remove the stale legacy fixtures. Evidence: `tests/basis` and `tests/errors` are gone.
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
- [ ] CI: nothing in the repository runs `scripts/validate.py` on a schedule or per change (there is no
  `.github/workflows`).
- [ ] More negative fixtures per stage (lexer, name resolution and native entry have 1 to 3 each). Mine
  visibility and cycle checks ([§12](../../specs/12-modules.md)), literal range and separator rules
  ([§1](../../specs/01-lexical.md)) and reserved words ([appendix A](../../specs/appendix-a-reserved.md)).
- [ ] One negative fixture per specified but unimplemented feature (protocols, contracts, effects,
  annotations) that pins today's diagnostic, so silent acceptance is caught when support is added.
- [ ] Audit platform-specific negative tests: Windows behavior (UTF-8 arguments, `/MT`) is tested on
  Windows ARM64; check Linux and macOS.

## Open questions

None.

## Out of scope

Running the existing suites; that is the job of `scripts/validate.py` and the platform plans.

## Progress log

- 2026-11-12: converted to the plan template; the optimization-equivalence and legacy-fixture items were
  already done and are checked.

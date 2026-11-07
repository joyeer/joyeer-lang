# Test Coverage Audit

> **Status:** plan document, not normative. Snapshot of test coverage against
> the spec and [supported features](../impl/supported-features.md). Counts come
> from `TEST(...)` macros and fixture file listings; none of the tests were run
> for this audit.

## What exists

| Layer | Evidence |
|---|---|
| GoogleTests, compiler | about 460 cases: semantic analysis 78, IR lowering 61, parser 56, LLVM 50, type checking 40, lexer 38, C interop 35, IR 29, module 28, name resolution 26 |
| GoogleTests, runtime | runtime, host and argument tests; the C filesystem and process tests were removed with that C code, and the host operations are now tested through `SystemTests` |
| Native fixtures | 79 files in `tests/native`, mostly checked-arithmetic and bounds traps, ownership, and `regression_*` programs |
| Negative fixtures | semantic-analysis 13, type-checking 17, parser 6, native 3, lexer 1, name-resolution 1 |
| Leak detection | `src/runtime/entry.c` reads `joyeer_runtime_active_allocations()` at exit, so native fixtures check allocation balance |
| Optimization | one `-O2` script (`scripts/cmake/verifyOptimizedNativeFailure.cmake`) and two fixtures (`optimized_bounds`, `optimized_overflow`) |

This is a solid base. The gaps are mostly in systematic and adversarial testing.

## Gaps, highest value first

1. **Differential testing across optimization levels (done).**
   `NativeOptimizationEquivalence_<fixture>` runs 62 self-contained
   `tests/native` fixtures at `-O0` to `-O3` and requires identical exit
   status, stdout, and stderr
   ([script](../../scripts/cmake/verifyOptimizationEquivalence.cmake)). Fixtures
   needing arguments, a working directory, or input files are excluded.
2. **No fuzzing or grammar-based generation.** Nothing for the lexer, parser,
   or type checker. The lexer has one negative fixture, name resolution has one,
   and the parser has five. Start with a byte-mutation fuzzer for the lexer and
   parser that checks only "no crash, no hang, a diagnostic or a valid AST".
3. **No sanitizer builds.** There is no ASan/UBSan/MSVC AddressSanitizer
   configuration for the compiler or the C runtime. A CMake option plus one
   validation row would cover it. The [runtime plan](runtime-in-joyeer.md)
   leans on this for the Joyeer runtime, whose address code is unchecked.
4. **No CI configuration in the repo.** There is no `.github/workflows`.
   `scripts/validate.py` is the gate, but nothing runs it on a schedule or per
   change.
5. **Move and destruction paths rely on hand-written cases.** Ownership is the
   riskiest area for a language with no GC. Cases exist (`use_after_consume`,
   `consuming`, `projection_consume`, `regression_flow_*`). Add generated
   programs that combine moves, loops, branches, early returns, and `?`
   propagation, run with the allocation-balance check. One generator would
   cover this whole family.
6. **Stale legacy fixtures (done).** `tests/basis` and `tests/errors` have been
   removed.
7. **Thin negative coverage per stage.** Lexer, name resolution, and native
   entry checks each have 1 to 3 negative fixtures. Spec sections to
   mine for missing cases: visibility and cycle checks (`12-modules.md`),
   literal range and separator rules (`01-lexical.md`), and reserved words
   (`15-reserved.md`).
8. **Spec-to-test traceability is absent.** No test names or comments cite spec
   sections, so coverage cannot be measured. Add a `// spec: 04-memory.md §4.2`
   convention to new tests, and a small script that lists spec sections with
   no citations.
9. **Unimplemented chapters have no expected-rejection tests.** Protocols,
   contracts, effects, and annotations are specified but not implemented.
   Add one negative fixture per unsupported feature that pins today's
   diagnostic, so silent acceptance is caught when support is added.
10. **Few platform-specific negative tests visible.** Windows-specific
    behavior (UTF-8 arguments, `/MT`) is documented as tested on Windows ARM64.
    Whether the same checks exist for Linux and macOS was not audited.

## Suggested order

1. Differential `-O0`/`-O2` run over `tests/native` (item 1).
2. Delete or port the legacy fixtures (item 6, done).
3. Add a sanitizer option and a validation row (item 3).
4. Add the parser/lexer fuzzer (item 2).
5. Add the ownership program generator (item 5).
6. Add the spec-citation convention and a coverage script (item 8).

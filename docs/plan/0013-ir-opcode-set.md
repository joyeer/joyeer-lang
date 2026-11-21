# 0013: A smaller, described IR opcode set

- Status: done (updated 2026-11-21); every item is verified on `feat/ir-opcode-descriptors`, not yet merged
- Decisions: [0028](../decisions/0028-ir-opcode-set.md); builds on
  [0025](../decisions/0025-library-collection-capabilities.md), [0026](../decisions/0026-library-array.md) and
  [0027](../decisions/0027-library-dict.md)
- Spec: none. The IR is not part of the language; [IR](../impl/ir.md) changes with each stage.

## Goal

Every IR opcode is described by one table row, the families that compile to the same code are merged, String
operations leave the scalar opcodes, and the opcodes are numbered densely. The result is 48 opcodes, each with one
stated shape, so a new opcode is one row plus its own verifier check and backend arm.

## Definition of done

- `compiler/ir/opcode.cpp` holds one descriptor row per opcode. `producesValue`, `isTerminator`, `opcodeName`,
  the verifier's shape and result checks, its runtime authorization, and the dumper's immediates read it.
- The merged opcodes of 0028 stage 2 exist and the old ones are gone. Float `!=` stays unordered.
- The arithmetic and comparison opcodes accept only scalars. String operations are calls or inlinable library
  code, within the cost gates of 0028 stage 3.
- Opcode values are dense and the table has no gaps.
- Each stage passed an `ir-backend-reviewer` pass and `python3 scripts/validate.py` in Debug and Release.

## Current state

Stage 0 removed `logicalAnd` (wire 26, [#33](https://github.com/joyeer/joyeer-lang/pull/33)). Stages 1 to 4 are
implemented on `feat/ir-opcode-descriptors`: `OpcodeDescriptor` rows in `compiler/ir/opcode.cpp` drive the helpers,
the verifier's shape, result, modifier and authorization checks, and the dumper; the families are merged (wire 27);
the opcodes are numbered densely in table order (wire 28); and String operations are calls to the collections
prelude, so the String opcodes are gone and `add`, `sub`, `mul` and `compare` take only scalars (48 opcodes, wire 29).
`scripts/validate.py` passes in Debug and Release. `Instruction::integerValue` still has four meanings; the row's
immediate kind names which one.

## Todo

### Stage 0: remove `logicalAnd`

- [x] Remove the opcode, reject slot 23, bump the wire to 26. Evidence: commit 902a0e7,
  `IRModelTest.RemovedLogicalAndWireSlotIsRejectedAndNeverReused`, Debug CTest.
- [x] Release CTest and `scripts/validate.py`. Evidence: Debug and Release pass at 025d80b; see the 2026-11-21
  validation entries in the progress log.

### Stage 1: one descriptor table (no wire change)

- [x] Add `OpcodeDescriptor` (spelling, operand and target counts, result rule, immediate kind, terminator,
  runtime-only) and one row per live opcode; tests pin every row. Evidence: `OpcodeDescriptorTest.*` in
  `tests/unittests/compiler/opcode_test.cpp`, including every slot up to the last having a row unless removed.
- [x] `producesValue`, `isTerminator` and `opcodeName` read the table. Evidence: `HelpersReadTheTable`,
  `RemovedAndOutOfRangeSlotsHaveNoRow`.
- [x] The verifier rejects unknown and removed opcodes by table lookup, checks fixed shapes and the result rule
  from the table once, and gates runtime-only opcodes from the table. Evidence: the shape, order, variadic and
  `EveryRuntimeOnlyOpcodeNeedsRuntimeAuthorization` tests; existing verifier tests unchanged and passing.
- [x] The dumper prints immediates by kind; existing dump text is unchanged. Evidence: `DumpTextIsUnchanged`
  passes against both the new dumper and the pre-table `dump.cpp`.
- [x] `ir-backend-reviewer` pass. Evidence: no behaviour change found; its low findings (completeness, authorization,
  order, variadic and dump tests; four doc gaps) are addressed in the same change.
- [x] Debug CTest. Evidence: full `-j8` run passed except four timeouts (ValidationScriptTests, Fern.Manifest,
  Fern.Workspace, Compression.Interoperability), each of which passed when rerun alone.
- [x] Release CTest and `scripts/validate.py`. Evidence: Debug and Release pass at 025d80b; see the 2026-11-21
  validation entries in the progress log.

### Stage 2: merge families

- [x] `literal`, `compare` (Float `!=` unordered), `convert`, `ret`, `typeHelper`, `trapIf` (runtime-only,
  message in IR and wire), with the predicate, mode and kind in `Instruction::modifier` rather than `integerValue`;
  wire 27. Evidence: `OpcodeDescriptorTest.*` (rows, modifiers, literal ranges, `ret`, `trap_if`, wire round trip).
- [x] Dump-text and opcode-count tests translated to the new spellings, and the diffs inspected. Evidence: the
  lowering, collection-role and dict-role suites pass; counts that now include Bool and unit literals count integer
  literals (`integerLiteralCount`), and comparisons count by predicate (`compareCount`).
- [x] `compare` requires a Bool result and scalar or String operands (found by the stage 2 review). Evidence:
  `CompareTakesScalarOrStringOperandsAndYieldsABool`; no lowered program in CTest is rejected.
- [x] Float predicates pinned. Evidence: `LLVMCompareTest.FloatingPredicatesAreOrderedExceptNotEqual` and
  `E2EFloatingComparisonsO0`-`O3`.
- [x] `ir-backend-reviewer` pass. Evidence: no critical or high findings; its medium and low findings are addressed in
  the same change, except a noncopyable `type_helper clone` test, which already fails through the shared clone check.
- [x] Debug CTest. Evidence: 2033 tests at `-j6`; only Fern.Workspace timed out, and it passed alone.
- [x] Release CTest and `scripts/validate.py`. Evidence: Debug and Release pass at 025d80b; see the 2026-11-21
  validation entries in the progress log.

### Stage 4: dense numbering (done before stage 3)

- [x] Renumber in table order, drop the removed-slot checks, bound by the table size, bump the wire (28). Evidence:
  `OpcodeDescriptorTest.EveryOpcodeHasItsPinnedRow` (each value equals its row index, wire pinned at 28),
  `OpcodeValuesAreDense`, `ValuesOutsideTheTableHaveNoRow`, and the `rowsAreDense` static assert.
- [x] Rewrite the raw-slot tests; update `backend_abi_test.c`, `backend.md` and `c-interop.md`. Evidence:
  `IRModelTest.OpcodeValuesOutsideTheTableAreRejected`, `RemovedTypeKindSlotsAreRejectedAndNeverReused`, and
  `LLVMBackendABITest.RejectsAnOpcodeOutsideTheTable` through the backend C ABI.
- [x] `ir-backend-reviewer` pass. Evidence: no critical or high findings; its findings are addressed in the same
  change (comment on what the static assert covers, wire pin, removed tautological checks, the C ABI test, wording).
- [x] Debug CTest. Evidence: 2031 tests at `-j6`; only Fern.Workspace timed out, and it passed alone.
- [x] Release CTest and `scripts/validate.py`. Evidence: Debug and Release pass at 025d80b; see the 2026-11-21
  validation entries in the progress log.

### Stage 3: String operations leave the scalar opcodes

- [x] Choose the mechanism, recorded in 0028. Evidence: the user chose a String projection with the bundled prelude
  (2026-11-20). The projection is two reserved C-binding intrinsics, not an opcode, because a C binding already
  receives a String as its data pointer and count; `utf8()` became an `Array` method instantiated for `[UInt8]`
  only, and the five String functions are collected on first use, both so that no prelude signature or body reaches
  modules, such as runtime core units, that do not use them.
- [x] `stringUtf8`, String `add` and comparisons become calls; `count` and `subscript` become inlinable library code;
  wire 29. Evidence: `OpcodeDescriptorTest.EveryOpcodeHasItsPinnedRow` (48 rows, wire 29),
  `CompareTakesScalarOperandsAndYieldsABool`, `ArithmeticTakesIntegerOrFloatingOperands`,
  `IRLoweringTest.LowersStringCountAndSubscript`, `LowersStringUtf8ToOwnedByteArray` and
  `LowersStringOperatorsToPreludeCalls`, each observed failing before the change; tests that located String
  operations by opcode now find the prelude calls.
- [x] The -O0/-O2 differential fixture. Evidence: `tests/e2e/string_operations.joyeer` (concatenation with empty
  sides and `+=`, `==`, `!=` and the orderings on equal, different and different-length Strings, reads at 0 and
  count - 1, `utf8()`, and a compile-time String ordering against the same one at run time), `E2EStringOperationsO0`
  to `O3`, passing with the pre-stage-3 compiler and with this one; reads at -1 and count are the
  `E2ECheckedString*` trap fixtures at every level.
- [x] The trap message and location. Evidence: `string index out of bounds` is unchanged (`E2ECheckedString*`).
  After `opt -passes=always-inline` on `checked_string_negative_index.joyeer` built with `-g`, both bounds panics
  are at `collections.joyeer` lines 243 and 246, inlined at the read (line 4, column 18), the subscript span the
  backend's trap used before. `LLVMBackendTest.StringByteReadsCallTheInlinedPreludeFromTheirLocation` pins the
  call's location and the `alwaysinline` callee.
- [x] The -O0 cost gate (0026's limits, Windows x64). Evidence: baseline d7a246f against this tree, both built with the
  same options (Debug compiler for -O0, Release for -O2), `benchmark_strings.py`, 30 interleaved repetitions, paired
  bootstrap: -O0 `string_bytes` 1.119 [1.113, 1.127], concat 0.967, compare 0.997, `utf8` 1.009, all within 1.15
  (`string_bytes` was 1.402 [1.395, 1.432] before String role parameters stopped being spilled). Sizes: unchanged
  except `string_utf8` +512 bytes at -O0 and -O2; `hello` unchanged at both. `hello` front end (`--emit-llvm`, 30
  paired runs) 1.012. Raw reports in `out/perf-gate/string-ops`.
- [x] The -O2 cost gate. Evidence: concat 0.952, compare 1.004 [0.995, 1.008] and `utf8` 1.002 pass; `string_bytes`
  -O2 is 1.023 [1.007, 1.052] (median within 1.03, lower bound above 1), and the 15-repetition run was disturbed
  ([0.40, 1.48]). The two compilers' inner byte loops are the same instructions after `opt -O2` (load, zero
  extension, checked add, compare and branch), so the difference is outside the loop's code, as in the 0027
  placement finding. Under 0026's rerun rule the user accepted this row on that evidence (2026-11-21).
- [x] `ir-backend-reviewer` and `type-system-reviewer` passes. Evidence: no critical or high findings. Addressed: an
  intrinsic accepting an initializing String parameter, which the backend would have read past
  (`CInteropTest.TheVerifierRejectsAnInitializingIntrinsicParameter`, observed failing first); a C binding accepted
  as a String role (`CollectionRoleTest.MalformedStringRolesAreRejected`, observed failing first); parameter labels
  now checked; tests for role validation, a prelude without the roles, collection on first use and from generic
  instances, and the projection intrinsics; an assertion that the reserved room holds; `name-resolution.md`.
- [x] Debug CTest. Evidence: after the review fixes, 2044 tests at `-j6`: all passed except the three known
  parallel-load timeouts (Fern.Manifest, Fern.Workspace, Compression.Interoperability) and the five `Moss.Fern.*`
  suites, which failed before building because all 128 Fern build-directory generations under the ignored
  `target/debug/moss.*` are taken by runs since 2026-11-07, not because of this change; the first full run of this
  stage, before that cap was reached, passed them (2033 of 2037, the other failure being the instantiation report
  whose prelude line moved, now regenerated).
- [x] Release CTest and `scripts/validate.py`. Evidence: Debug and Release pass at 025d80b; see the 2026-11-21
  validation entries in the progress log.

## Open questions

None. The three questions of 0028 are [resolved](../decisions/0028-ir-opcode-set.md#resolved-questions):
`destroyRange` and `extractPayload` stay, and the implicit traps stay in their opcodes rather than becoming
`trapIf`.

## Out of scope

New opcodes, and any change to what programs observe outside stage 3's measured String change.

## Progress log

- 2026-11-21: The user accepted answers to 0028's three open questions: keep `destroyRange`, keep `extractPayload`,
  and keep the implicit traps in their opcodes. Recorded in 0028 under "Resolved questions"; no code change.
- 2026-11-21: With the five full `target/debug/moss.*` folders cleared (by the user's choice),
  `scripts/validate.py --configs Debug` at 025d80b passes: build, CTest 2045 of 2045 and `fern test` 7 of 7. With the
  Release run below, every stage's validation item is checked.
- 2026-11-21: `scripts/validate.py --llvm-root C:\LLVM-22.1.8` at 025d80b (all stages), Windows AMD64. Release:
  build, CTest 2045 of 2045 and `fern test` 7 of 7 pass. Debug: build passes; CTest 2039 of 2045, where
  Fern.Workspace passed on the validator's rerun and the five `Moss.Fern.*` suites failed before building; `fern
  test` stopped at `moss.archive.tests` for the same reason. Each of the five ignored `target/debug/moss.*` folders
  already holds Fern's limit of 128 build directories, from runs since 2026-11-07, so this is not a change failure.
  The Debug row stays open until those folders are cleared and the validator is rerun.
- 2026-11-21: The user accepted the stage 3 -O2 `string_bytes` row (attributed to code placement); recorded in 0028.
  Every stage's remaining item is the Release CTest and `scripts/validate.py`.
- 2026-11-21: Stage 3 implemented (wire 29): String operations call the collections prelude, and the String opcodes
  are removed. Both reviews addressed; the -O0 gate passes after String role parameters stopped being spilled. Open:
  the -O2 `string_bytes` row (lower bound 1.007) needs the user's decision; Release CTest and `scripts/validate.py`.
- 2026-11-20: Stage 4 implemented (wire 28): 51 opcodes numbered densely in seven groups; all items checked except
  Release CTest and `scripts/validate.py`. Stage 3 waits for the user's choice of mechanism.
- 2026-11-20: Stage 2 implemented (wire 27): merged families, `Instruction::modifier`, `trap_if`, stricter `compare`;
  all items checked except Release CTest and `scripts/validate.py`. Stage 4 moves before stage 3: with dense
  numbering, stage 3 removes opcodes by deleting rows, and stage 3 starts with a design choice for the user.
- 2026-11-20: Stage 1 implemented (descriptor table, verifier, dumper, tests, docs); all items checked except
  Release CTest and `scripts/validate.py`.
- 2026-11-20: Plan created after 0028 was accepted; stage 0 item checked from #33.

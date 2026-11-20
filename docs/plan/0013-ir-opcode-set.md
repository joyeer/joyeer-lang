# 0013: A smaller, described IR opcode set

- Status: in progress (updated 2026-11-20)
- Decisions: [0028](../decisions/0028-ir-opcode-set.md); builds on
  [0025](../decisions/0025-library-collection-capabilities.md), [0026](../decisions/0026-library-array.md) and
  [0027](../decisions/0027-library-dict.md)
- Spec: none. The IR is not part of the language; [IR](../impl/ir.md) changes with each stage.

## Goal

Every IR opcode is described by one table row, the families that compile to the same code are merged, String
operations leave the scalar opcodes, and the opcodes are numbered densely. The result is about 48 opcodes, each
with one stated shape, so a new opcode is one row plus its own verifier check and backend arm.

## Definition of done

- `compiler/ir/opcode.cpp` holds one descriptor row per opcode. `producesValue`, `isTerminator`, `opcodeName`,
  the verifier's shape and result checks, its runtime authorization, and the dumper's immediates read it.
- The merged opcodes of 0028 stage 2 exist and the old ones are gone. Float `!=` stays unordered.
- The arithmetic and comparison opcodes accept only scalars. String operations are calls or inlinable library
  code, within the cost gates of 0028 stage 3.
- Opcode values are dense and the table has no gaps.
- Each stage passed an `ir-backend-reviewer` pass and `python3 scripts/validate.py` in Debug and Release.

## Current state

Stage 0 removed `logicalAnd` (wire 26, [#33](https://github.com/joyeer/joyeer-lang/pull/33)). Stages 1, 2 and 4
are implemented on `feat/ir-opcode-descriptors`: `OpcodeDescriptor` rows in `compiler/ir/opcode.cpp` drive the
helpers, the verifier's shape, result, modifier and authorization checks, and the dumper; the families are merged
(wire 27); and the 51 opcodes are numbered densely in table order (wire 28). What is left is stage 3: String still has
the scalar opcodes `string_utf8`, `count` and `subscript`, and String arms in `add` and `compare`. Its mechanism is a
design choice for the user. `Instruction::integerValue` still has four meanings; the row's immediate kind names which
one.

## Todo

### Stage 0: remove `logicalAnd`

- [x] Remove the opcode, reject slot 23, bump the wire to 26. Evidence: commit 902a0e7,
  `IRModelTest.RemovedLogicalAndWireSlotIsRejectedAndNeverReused`, Debug CTest.
- [ ] Release CTest and `scripts/validate.py`. Not run for stage 0.

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
- [ ] Release CTest and `scripts/validate.py`. Not run.

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
- [ ] Release CTest and `scripts/validate.py`. Not run.

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
- [ ] Release CTest and `scripts/validate.py`. Not run.

### Stage 3: String operations leave the scalar opcodes

- [ ] Choose the mechanism (a String projection opcode with a bundled prelude, or String as a library struct),
  recorded in 0028 or a new decision.
- [ ] `stringUtf8`, String `add` and comparisons become calls; `count` and `subscript` become inlinable library code.
- [ ] The -O0 cost gate, the -O0/-O2 differential fixture, and the trap message and location check of 0028.
- [ ] `ir-backend-reviewer` pass; `scripts/validate.py` in Debug and Release.

## Open questions

The three open questions of [0028](../decisions/0028-ir-opcode-set.md#open-questions): `destroyRange`,
`extractPayload`, and whether `trapIf` replaces implicit traps.

## Out of scope

New opcodes, and any change to what programs observe outside stage 3's measured String change.

## Progress log

- 2026-11-20: Stage 4 implemented (wire 28): 51 opcodes numbered densely in seven groups; all items checked except
  Release CTest and `scripts/validate.py`. Stage 3 waits for the user's choice of mechanism.
- 2026-11-20: Stage 2 implemented (wire 27): merged families, `Instruction::modifier`, `trap_if`, stricter `compare`;
  all items checked except Release CTest and `scripts/validate.py`. Stage 4 moves before stage 3: with dense
  numbering, stage 3 removes opcodes by deleting rows, and stage 3 starts with a design choice for the user.
- 2026-11-20: Stage 1 implemented (descriptor table, verifier, dumper, tests, docs); all items checked except
  Release CTest and `scripts/validate.py`.
- 2026-11-20: Plan created after 0028 was accepted; stage 0 item checked from #33.

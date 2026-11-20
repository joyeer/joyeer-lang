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
- Opcode values are dense and the table has no gaps (since stage 1 the verifier finds removed slots and the upper
  bound through the table).
- Each stage passed an `ir-backend-reviewer` pass and `python3 scripts/validate.py` in Debug and Release.

## Current state

Stage 0 removed `logicalAnd` (wire 26, [#33](https://github.com/joyeer/joyeer-lang/pull/33)). Stage 1 is
implemented on `feat/ir-opcode-descriptors`: `OpcodeDescriptor` rows in `compiler/ir/opcode.cpp` drive the helpers,
the verifier's slot, shape, result and authorization checks, and the dumper. The families are not merged yet, String
still has scalar opcodes, and the slots still have gaps. `Instruction::integerValue` still has four meanings; the
row's immediate kind names which one.

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

- [ ] `constant`, `compare` (Float `!=` unordered), `convert`, `return`, `typeHelper`, `trapIf` (runtime-only,
  message in IR and wire), each with a dedicated `Instruction` field rather than `integerValue`; wire bump.
- [ ] Dump-text tests regenerated with the project's tooling and the diffs inspected.
- [ ] `ir-backend-reviewer` pass; `scripts/validate.py` in Debug and Release.

### Stage 3: String operations leave the scalar opcodes

- [ ] Choose the mechanism (a String projection opcode with a bundled prelude, or String as a library struct),
  recorded in 0028 or a new decision.
- [ ] `stringUtf8`, String `add` and comparisons become calls; `count` and `subscript` become inlinable library code.
- [ ] The -O0 cost gate, the -O0/-O2 differential fixture, and the trap message and location check of 0028.
- [ ] `ir-backend-reviewer` pass; `scripts/validate.py` in Debug and Release.

### Stage 4: dense numbering

- [ ] Renumber in table order, drop the removed-slot checks, bound by the table size, bump the wire.
- [ ] Rewrite the raw-slot tests; update `backend_abi_test.c`, `backend.md` and `c-interop.md`.
- [ ] `ir-backend-reviewer` pass; `scripts/validate.py` in Debug and Release.

## Open questions

The three open questions of [0028](../decisions/0028-ir-opcode-set.md#open-questions): `destroyRange`,
`extractPayload`, and whether `trapIf` replaces implicit traps.

## Out of scope

New opcodes, and any change to what programs observe outside stage 3's measured String change.

## Progress log

- 2026-11-20: Stage 1 implemented (descriptor table, verifier, dumper, tests, docs); all items checked except
  Release CTest and `scripts/validate.py`.
- 2026-11-20: Plan created after 0028 was accepted; stage 0 item checked from #33.

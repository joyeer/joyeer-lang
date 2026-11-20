# 0028: A smaller, described IR opcode set

- Status: accepted (2026-11-20, by the user). Stage 0 (remove `logicalAnd`, wire 26) is implemented; stages 1-4
  are tracked in [plan 0013](../plan/0013-ir-opcode-set.md).
- Spec: none. The IR is not part of the language; [IR](../impl/ir.md) changes with each stage.
- Builds on [0025](0025-library-collection-capabilities.md), [0026](0026-library-array.md) and
  [0027](0027-library-dict.md), which moved Array and Dict out of the instruction set.

## Context

Before stage 0, `compiler/ir/opcode.h` had 63 opcodes in 71 wire slots. The set grew one feature at a time and was never reviewed as
a whole. A review on 2026-11-19 compared every opcode with what lowering emits (`irlowering.cpp`), what the verifier
checks (`compiler/ir/verifier/`) and what the backend generates (`native_functions.cpp`,
`native_collections.cpp`). It found four kinds of problem.

1. **Dead opcodes.** Lowering never emitted `logicalAnd`: `&&` and `||` lower to branches (`lowerLogical`), and two
   lowering tests checked that. The verifier, dumper and backend still handled it.
2. **String operations in the instruction set.** Array and Dict are library structs now, but String still has
   opcodes of its own, and generic arithmetic and comparison opcodes behave differently on String:
   - `count` is the byte length of a String, and `subscript` is the byte at an `Int` offset (a checked index and one
     inline load). Their generic names are left over from the removed builtin containers. The verifier checks
     them as `verifyStringCount` and `verifyStringSubscript`.
   - `stringUtf8` calls `joyeer_array_create_owned_abi` (`lib/Runtime/core/arrays.joyeer`).
   - `add` on String calls `joyeer_string_concat_abi` and allocates. It is the only arithmetic opcode with an owned
     result, so lowering special-cases `add && requiresDestroy` when it records ownership.
   - `equal` and `notEqual` on String call `joyeer_string_equal_abi`; the ordering opcodes call
     `joyeer_string_compare_abi`.

   The called functions are written in Joyeer (`lib/Runtime/core/strings.joyeer`, `text.joyeer`, `arrays.joyeer`)
   and exported with `@c_export` from the separately compiled runtime archive. Runtime core units do not use the
   String type: they take `(data: Int, count: Int)`. `count` and `subscript` have no runtime function; the
   backend emits them inline.
3. **Families that compile to the same code.** The backend handles these groups with one code path, and the
   opcodes in each group differ only in a parameter:
   - `integerConstant`, `byteConstant` and `booleanConstant` all become an LLVM integer constant from
     `integerValue`; the result type already says which one. `unitConstant` is the zero value of `Void`.
   - The six comparisons go through one `comparison()` function.
   - `convertChecked` and `convertWrapping` go through one `convert()` function and differ only in whether it
     traps.
   - `returnValue` and `returnVoid` are a return with one operand or none.
   - `cloneCallback` and `destroyCallback` share one arm with a flag.
   - `stackBlock` is a counted `stackAllocate` whose result is an `Int`.
   - `linearRangeGuard` is "trap with a fixed message when the operand is not zero".
4. **No single description of an opcode.** Each opcode's arity, result rule, immediate operand and restrictions are
   spread across `producesValue`, `isTerminator`, `opcodeName`, the dumper, the verifier's dispatch and the backend's
   switch. `Instruction::integerValue` means four different things depending on the opcode: a literal, a `TypeId`
   (`typeSize`, `slotAddress`, `stackBlock`, the collection capabilities, and `integerFits`'s target type), a
   constant index (`constantValue`), and a payload index (`extractPayload`, `payloadAddress`).

The opcode slots have been treated as permanent ("never renumber them, and keep removed slots unused"), leaving
holes at 23, 35-41 and 50. The wire format is private to the compiler and the backend, which ship together, and is
versioned (`wire::version`), and the backend rejects a module whose version does not match. The holes protect no
reader.

The goals that constrain the answer: the IR must stay easy to check (the verifier is the trust boundary in front of
the backend), the backend should not need to know about library types, and unoptimized code must not get slower
(0026 and 0027 set the cost gates for library containers).

## Options considered

1. **Keep the set and only document it.** No risk, but String stays in the instruction set, the families stay
   duplicated, and `integerValue` stays overloaded.
2. **Merge families, describe every opcode in one table, and move String to calls** (proposed). This removes about
   14 opcodes, and every opcode gets one row stating its shape. The scalar opcodes no longer know about String.
   String keeps its type kind, `stringConstant`, constant decoding, literal patterns and native ABI marshalling; it
   does not become a library type.
3. **A minimal core plus a generic `intrinsic` opcode** carrying a name, as LLVM does. Fewer opcodes, but each
   intrinsic still needs its own verifier and backend rules. Those rules move from a typed enum into string
   matching, which makes them harder to check exhaustively.

## Decision

Option 2, in stages. Each stage that changes the wire format bumps `wire::version`, and each stage needs an
`ir-backend-reviewer` pass before it lands.

- **Stage 0, done (wire 26).** Remove `logicalAnd`. The verifier rejects slot 23 like the other removed slots.
- **Stage 1: one descriptor table (no wire change).** Each opcode gets one row in `opcode.cpp` with its spelling,
  operand count (fixed or variadic), target count, result rule (none, always, or set by the direct callee or the
  function type), immediate kind (none, integer literal, floating or String literal text, type, constant index, or
  payload index), whether it is a terminator, and whether it is runtime-only. `producesValue`, `isTerminator` and
  `opcodeName` read the table. The verifier rejects a slot with no row, checks the shape and result rule from the
  table before running the opcode's own check, and takes runtime authorization from the row. The dumper prints the
  immediate by its kind. No opcode changes meaning in this stage.
- **Stage 2: merge families.** Each merge folds a parameter into one opcode:

  | Today | Becomes |
  |---|---|
  | `integerConstant`, `byteConstant`, `booleanConstant`, `unitConstant` | `constant` (literal immediate, typed by the result) |
  | `less`, `lessEqual`, `greater`, `greaterEqual`, `equal`, `notEqual` | `compare` with a predicate |
  | `convertChecked`, `convertWrapping` | `convert` with a mode |
  | `returnValue`, `returnVoid` | `return` with 0 or 1 operands |
  | `cloneCallback`, `destroyCallback` | `typeHelper` with a kind |
  | `linearRangeGuard` | `trapIf` with a message |

  The predicate, mode and kind take a dedicated `Instruction` field, not `integerValue`. Details each merge must
  keep:
  - `compare` keeps today's floating-point semantics: `!=` is unordered (true for NaN) and the other predicates
    are ordered.
  - `trapIf` stays runtime-only, as `linearRangeGuard` is today. Its message becomes IR and wire data.

  `stackBlock` is not merged. It yields 0 for `Void` or a zero count, allows an empty literal of a `Never`
  element, and is allowed outside runtime functions as proven literal storage, which anchors the protocol in
  `verifier/literals.cpp`. Expressing it as `stackAllocate` plus the runtime-only `pointerToInteger` would loosen
  that authorization and turn a one-instruction proof into a two-instruction pattern.
- **Stage 3: String operations leave the scalar opcodes.** `stringUtf8`, and the String arms of `add` and the
  comparisons, become calls; `count` and `subscript` become inlinable library code. Then the arithmetic and
  comparison opcodes accept only scalars, never allocate, and never produce owned values, and lowering's
  `add && requiresDestroy` ownership special case goes away.

  The mechanism is open, because the obvious one does not work. The runtime's String functions are `@c_export`
  functions in a separate archive; a user module sees only their declarations. The verifier rejects
  `alwaysInline` on external functions, and there is no LTO. Inlinable wrappers must therefore live in a bundled
  source prelude, as Array's do (`collections.joyeer`), and the prelude needs a String's data address and byte
  count. That takes either one String projection opcode or intrinsic, which then stays, or making String a library
  struct, which is a much larger change and would need its own decision. Stage 3 starts by choosing between them.

  The wrappers take their String parameters by borrowing, so -O0 gains no copies. Acceptance:
  - The unoptimized cost gate of 0026 and 0027, including a `count`/`subscript` byte loop at -O0. The -O0 pipeline
    runs the always-inliner but not SROA or mem2reg, so today's single `extractvalue` can become spills and
    reloads.
  - A fixture run at -O0 and -O2 with stdout, exit code and trap text compared between levels. It covers concat
    (including empty strings), `==` and `<` on equal and different-length strings, `subscript` at -1, 0, count-1
    and count, and `utf8()`.
  - The `string index out of bounds` trap keeps its message, and its reported source location is checked after
    inlining.

  Literal patterns on String values in `switchPattern` keep using the runtime equality function inside the backend.
- **Stage 4: dense numbering.** Drop "never renumber". Number the opcodes densely in table order, so the table has
  no gaps, and bump the wire version. Since stage 1 the verifier finds removed slots and the upper bound through
  the table, so stage 4 only removes the table's gap handling (`rowBySlot`). From then on, removing an opcode means
  deleting its row and bumping the version. The same stage must also:
  - rewrite the raw-slot tests (`ir_test.cpp`, `collection_roles_test.cpp`, `dict_roles_test.cpp`), which would
    otherwise match live opcodes instead of removed ones;
  - update the literal version in `backend_abi_test.c` and the version stated in `backend.md` and `c-interop.md`.

Kept as they are, because each says something that no other opcode can: `stackAllocate`, `zeroInitialize`, `load`,
`store`, `copyValue`, `take`, `destroy`; scalar `add`, `subtract`, `multiply`, `divide`, `remainder`, `bitAnd`,
`bitOr`, `bitXor`, `shiftLeft`, `shiftRight`, `integerFits`; `floatingConstant`, `stringConstant`,
`constantValue`; construct, extract and address for structs and enums; `call`, `callIndirect`,
`functionAddress`; `branch`, `conditionalBranch`, `switchPattern`, `unreachable`; and the runtime-only `typeSize`,
`typeAlignment`, `integerAddress`, `pointerToInteger`, `slotAddress`, `stackBlock`, `cloneAt` and `destroyRange`. Negation, `!`
and `~` stay lowered to `subtract`, a comparison and `bitXor`; they get no opcodes.

The result is about 48 opcodes in seven groups: constants, memory and ownership, scalar operations, aggregates,
calls, control flow, and runtime-only memory operations.

## Consequences

- A new opcode is one table row plus its own verifier check and backend arm. Fixed-arity and result mistakes are
  caught generically; the immediate kind drives the dumper, and each opcode's check still verifies its immediate.
- The backend no longer needs to know String's layout for arithmetic or comparison. That is the last
  library-type knowledge left in the scalar opcodes.
- IR dumps change spelling in stages 2 and 3. Tests that match dump text must be regenerated with the project's
  tooling, and the diffs inspected.
- Each stage changes the verifier and the backend together, so each stage is reviewed on its own and runs the full
  `scripts/validate.py` gate.
- Stage 3 is the only stage that can change generated code. Its acceptance is the cost measurement, not just the
  tests.

## Open questions

1. `destroyRange` can be written in library code as a loop over `destroy_at` guarded by `needs_destroy`. Keep it
   for code size, or remove it?
2. `extractPayload` can be expressed as a spill, `payloadAddress` and `load`, which the backend already does
   internally. It is kept so every aggregate has construct, extract and address; is that rule worth one opcode?
3. Should `trapIf` also replace the implicit traps in arithmetic, shifts and conversions? That would make each
   trap visible in the IR, at the cost of larger IR.

## Evidence

- The opcode review of 2026-11-19, which counted each opcode's references in lowering, verifier, backend and tests.
  `logicalAnd` was the only opcode with no producer.
- 0026 and 0027 removed eight opcodes and two type kinds without changing what programs observe. They showed that
  library code using the collection capabilities can replace builtin opcodes within the cost gates.

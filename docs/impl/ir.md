# Joyeer IR — Typed Backend-Neutral Lowering

> **Status:** Core model and JSON-parser MVP lowering implemented and wired
> into `--lang=v0.1`.
> **Input:** `typing::TypeCheckedModel`.
> **Output:** a structurally verified `ir::Module`.

---

## 1. Boundary

Joyeer IR is the replacement frontend's first backend-neutral representation.
It does not reuse legacy `IRGen`, runtime `Type`, bytecode instructions, or VM
objects. The public model is in `include/joyeer/ir/ir.h`; typed AST lowering is
in `include/joyeer/compiler/irlowering.h` and
`lib/compiler/irlowering.cpp`.

A successful `--lang=v0.1` compilation stores the result in
`SourceFile::joyeerIR`. The LLVM/native backend consumes it for
`--emit-llvm` and `-o`; see [native.md](native.md).

---

## 2. Representation

The module owns:

- a deterministic snapshot of canonical compile-time `TypeId` names;
- concrete user `struct` field definitions;
- user enum and instantiated `Optional<T>` / `Result<T,E>` case definitions;
- external built-ins and source functions;
- functions containing typed values, stack addresses, basic blocks, and
  source-spanned instructions.

Values and addresses are different `ValueCategory` values. Mutable local
storage uses `alloc_stack`, `load`, and `store`. Ordinary parameters enter by
value and are copied to a local slot. `inout` parameters and arguments are
addresses, preserving aliasing instead of silently copying them.

This is intentionally allocation-based rather than SSA. LLVM's `mem2reg` can
promote eligible slots after lowering, while source variables retain simple
and debuggable semantics in the language IR.

---

## 3. Implemented instructions

The current instruction set covers:

- scalar and string constants;
- stack allocation, zero initialization, load, and store;
- explicit `copy`, `take`, and `destroy` ownership operations;
- MVP arithmetic, comparison, and logical operations;
- direct source and external calls;
- unconditional/conditional branches, returns, and unreachable;
- struct construction, field address, and field extraction;
- enum construction and payload extraction;
- `String`/collection count and value/address subscript operations;
- high-level recursive pattern switching.

`if` expressions merge values through a typed temporary slot. `while` emits a
header, body, exit, and back edge. A `Never` branch terminates without adding a
false fallthrough edge.

Optional promotion is explicit in IR: a type-checked conversion from `T` to
`T?` emits `Optional.Some(T)` at binding, field, argument, arm-merge, or return
boundaries. `nil` emits `Optional.None`.

Heap-backed and recursively nontrivial values have explicit ownership in the
IR. Borrowed values are cloned before entering owned storage; owned temporaries
are moved when possible; overwriting storage destroys the previous value; and
scope exits destroy owned storage and live temporaries in reverse order.
Early returns clean every active scope before transferring the result to the
caller. A typed local declared without an initializer uses `zero_init` when its
type requires destruction, making cleanup safe while Stage 5 rejects any
source-level read before initialization.

---

## 4. Pattern representation

`switch_pattern` is a high-level terminator whose cases retain source order.
Patterns recursively represent:

- wildcard and binding positions;
- integer, Boolean, string, and byte literals;
- enum cases with nested payload patterns.

Each arm has its own basic block. Payload bindings use `extract_payload`, then
a local stack slot. Exhaustiveness is already guaranteed by the type checker;
the future LLVM backend lowers the high-level pattern list into tag and value
tests without reconstructing source semantics.

---

## 5. Verification

`ir::Verifier` rejects:

- duplicate or unknown type/function/block/value identifiers;
- malformed instruction operand/target counts;
- invalid value/address categories;
- load/store, return, operator, call, field, aggregate, or payload type
  mismatches;
- invalid `copy`, `take`, `destroy`, or `zero_init` operand categories/types;
- malformed recursive patterns;
- blocks without terminators or instructions after a terminator.

Lowering returns an `ir-lowering.verification-failed` diagnostic if generated
IR does not verify. The CLI never advances a malformed module.

The deterministic textual form is produced by `ir::dump` and includes types,
aggregate definitions, signatures, blocks, instructions, symbols, source
spans, and recursive patterns.

Direct validation:

```pwsh
ctest --test-dir build -L "ir|ir-lowering" --output-on-failure
```

The lowering suite includes the complete JSON-parser MVP fixture.

---

## 6. Remaining backend work

The current IR/native pipeline intentionally leaves these to later commits:

- source-level `borrowing`/`consuming`/`initializing` conventions and
  `consume` use-after-move analysis;
- copy-elision and ABI tuning for large aggregates;
- enum niche optimization and a stable public ABI;
- an explicit LLVM optimization pipeline;
- source-level debug information.

None of those should be implemented by extending the compatibility VM lane.
